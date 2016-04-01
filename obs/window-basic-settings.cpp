/******************************************************************************
    Copyright (C) 2013-2014 by Hugh Bailey <obs.jim@gmail.com>
                               Philippe Groarke <philippe.groarke@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <obs.hpp>
#include <util/util.hpp>
#include <util/lexer.h>
#include <graphics/math-defs.h>
#include <initializer_list>
#include <sstream>
#include <QLineEdit>
#include <QMessageBox>
#include <QCloseEvent>
#include <QFileDialog>
#include <QDirIterator>
#include <QVariant>
#include <QTreeView>
#include <QStandardItemModel>
#include <QSpacerItem>

#include "audio-encoders.hpp"
#include "hotkey-edit.hpp"
#include "source-label.hpp"
#include "obs-app.hpp"
#include "platform.hpp"
#include "properties-view.hpp"
#include "qt-wrappers.hpp"
#include "window-basic-main.hpp"
#include "window-basic-settings.hpp"

#include <util/platform.h>

using namespace std;

// Used for QVariant in codec comboboxes
namespace {
static bool StringEquals(QString left, QString right)
{
	return left == right;
}
struct FormatDesc {
	const char *name = nullptr;
	const char *mimeType = nullptr;
	const ff_format_desc *desc = nullptr;

	inline FormatDesc() = default;
	inline FormatDesc(const char *name, const char *mimeType,
			const ff_format_desc *desc = nullptr)
			: name(name), mimeType(mimeType), desc(desc) {}

	bool operator==(const FormatDesc &f) const
	{
		if (!StringEquals(name, f.name))
			return false;
		return StringEquals(mimeType, f.mimeType);
	}
};
struct CodecDesc {
	const char *name = nullptr;
	int id = 0;

	inline CodecDesc() = default;
	inline CodecDesc(const char *name, int id) : name(name), id(id) {}

	bool operator==(const CodecDesc &codecDesc) const
	{
		if (id != codecDesc.id)
			return false;
		return StringEquals(name, codecDesc.name);
	}
};
}
Q_DECLARE_METATYPE(FormatDesc)
Q_DECLARE_METATYPE(CodecDesc)

/* parses "[width]x[height]", string, i.e. 1024x768 */
static bool ConvertResText(const char *res, uint32_t &cx, uint32_t &cy)
{
	BaseLexer lex;
	base_token token;

	lexer_start(lex, res);

	/* parse width */
	if (!lexer_getbasetoken(lex, &token, IGNORE_WHITESPACE))
		return false;
	if (token.type != BASETOKEN_DIGIT)
		return false;

	cx = std::stoul(token.text.array);

	/* parse 'x' */
	if (!lexer_getbasetoken(lex, &token, IGNORE_WHITESPACE))
		return false;
	if (strref_cmpi(&token.text, "x") != 0)
		return false;

	/* parse height */
	if (!lexer_getbasetoken(lex, &token, IGNORE_WHITESPACE))
		return false;
	if (token.type != BASETOKEN_DIGIT)
		return false;

	cy = std::stoul(token.text.array);

	/* shouldn't be any more tokens after this */
	if (lexer_getbasetoken(lex, &token, IGNORE_WHITESPACE))
		return false;

	return true;
}

static inline bool WidgetChanged(QWidget *widget)
{
	return widget->property("changed").toBool();
}

static inline void SetComboByName(QComboBox *combo, const char *name)
{
	int idx = combo->findText(QT_UTF8(name));
	if (idx != -1)
		combo->setCurrentIndex(idx);
}

static inline void SetComboByValue(QComboBox *combo, const char *name)
{
	int idx = combo->findData(QT_UTF8(name));
	if (idx != -1)
		combo->setCurrentIndex(idx);
}

static inline QString GetComboData(QComboBox *combo)
{
	int idx = combo->currentIndex();
	if (idx == -1)
		return QString();

	return combo->itemData(idx).toString();
}

static int FindEncoder(QComboBox *combo, const char *name, int id)
{
	CodecDesc codecDesc(name, id);
	for(int i = 0; i < combo->count(); i++) {
		QVariant v = combo->itemData(i);
		if (!v.isNull()) {
			if (codecDesc == v.value<CodecDesc>()) {
				return i;
				break;
			}
		}
	}
	return -1;
}

static CodecDesc GetDefaultCodecDesc(const ff_format_desc *formatDesc,
		ff_codec_type codecType)
{
	int id = 0;
	switch (codecType) {
	case FF_CODEC_AUDIO:
		id = ff_format_desc_audio(formatDesc);
		break;
	case FF_CODEC_VIDEO:
		id = ff_format_desc_video(formatDesc);
		break;
	default:
		return CodecDesc();
	}

	return CodecDesc(ff_format_desc_get_default_name(formatDesc, codecType),
			id);
}

#ifdef _WIN32
void OBSBasicSettings::ToggleDisableAero(bool checked)
{
	SetAeroEnabled(!checked);
}
#endif

static void PopulateAACBitrates(initializer_list<QComboBox*> boxes)
{
	auto &bitrateMap = GetAACEncoderBitrateMap();
	if (bitrateMap.empty())
		return;

	vector<pair<QString, QString>> pairs;
	for (auto &entry : bitrateMap)
		pairs.emplace_back(QString::number(entry.first),
				obs_encoder_get_display_name(entry.second));

	for (auto box : boxes) {
		QString currentText = box->currentText();
		box->clear();

		for (auto &pair : pairs) {
			box->addItem(pair.first);
			box->setItemData(box->count() - 1, pair.second,
					Qt::ToolTipRole);
		}

		box->setCurrentText(currentText);
	}
}

void OBSBasicSettings::HookWidget(QWidget *widget, const char *signal,
		const char *slot)
{
	QObject::connect(widget, signal, this, slot);
	widget->setProperty("changed", QVariant(false));
}

#define COMBO_CHANGED   SIGNAL(currentIndexChanged(int))
#define EDIT_CHANGED    SIGNAL(textChanged(const QString &))
#define CBEDIT_CHANGED  SIGNAL(editTextChanged(const QString &))
#define CHECK_CHANGED   SIGNAL(clicked(bool))
#define SCROLL_CHANGED  SIGNAL(valueChanged(int))

#define GENERAL_CHANGED SLOT(GeneralChanged())
#define STREAM1_CHANGED SLOT(Stream1Changed())
#define OUTPUTS_CHANGED SLOT(OutputsChanged())
#define AUDIO_RESTART   SLOT(AudioChangedRestart())
#define AUDIO_CHANGED   SLOT(AudioChanged())
#define VIDEO_RESTART   SLOT(VideoChangedRestart())
#define VIDEO_RES       SLOT(VideoChangedResolution())
#define VIDEO_CHANGED   SLOT(VideoChanged())
#define ADV_CHANGED     SLOT(AdvancedChanged())
#define ADV_RESTART     SLOT(AdvancedChangedRestart())

OBSBasicSettings::OBSBasicSettings(QWidget *parent)
	: QDialog          (parent),
	  main             (qobject_cast<OBSBasic*>(parent)),
	  ui               (new Ui::OBSBasicSettings)
{
	string path;

	ui->setupUi(this);

	PopulateAACBitrates({ui->advOutTrack1Bitrate, ui->advOutTrack2Bitrate,
			ui->advOutTrack3Bitrate, ui->advOutTrack3Bitrate});

	ui->listWidget->setAttribute(Qt::WA_MacShowFocusRect, false);

	auto policy = ui->audioSourceScrollArea->sizePolicy();
	policy.setVerticalStretch(true);
	ui->audioSourceScrollArea->setSizePolicy(policy);

	HookWidget(ui->language,             COMBO_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->theme, 		     COMBO_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->warnBeforeStreamStart,CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->warnBeforeStreamStop, CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->advOutFTLIngestLoc,   COMBO_CHANGED, OUTPUTS_CHANGED);
	HookWidget(ui->advOutFFURL,          EDIT_CHANGED,   OUTPUTS_CHANGED);
	HookWidget(ui->advOutFFVBitrate,     SCROLL_CHANGED, OUTPUTS_CHANGED);
	HookWidget(ui->advOutFFUseRescale,   CHECK_CHANGED,  OUTPUTS_CHANGED);
	HookWidget(ui->advOutFFRescale,      CBEDIT_CHANGED, OUTPUTS_CHANGED);
	HookWidget(ui->advOutFFABitrate,     SCROLL_CHANGED, OUTPUTS_CHANGED);
	HookWidget(ui->advOutFFTrack1,       CHECK_CHANGED,  OUTPUTS_CHANGED);
	HookWidget(ui->advOutFFTrack2,       CHECK_CHANGED,  OUTPUTS_CHANGED);
	HookWidget(ui->advOutFFTrack3,       CHECK_CHANGED,  OUTPUTS_CHANGED);
	HookWidget(ui->advOutFFTrack4,       CHECK_CHANGED,  OUTPUTS_CHANGED);
	HookWidget(ui->advOutTrack1Bitrate,  COMBO_CHANGED,  OUTPUTS_CHANGED);
	HookWidget(ui->advOutTrack1Name,     EDIT_CHANGED,   OUTPUTS_CHANGED);
	HookWidget(ui->advOutTrack2Bitrate,  COMBO_CHANGED,  OUTPUTS_CHANGED);
	HookWidget(ui->advOutTrack2Name,     EDIT_CHANGED,   OUTPUTS_CHANGED);
	HookWidget(ui->advOutTrack3Bitrate,  COMBO_CHANGED,  OUTPUTS_CHANGED);
	HookWidget(ui->advOutTrack3Name,     EDIT_CHANGED,   OUTPUTS_CHANGED);
	HookWidget(ui->advOutTrack4Bitrate,  COMBO_CHANGED,  OUTPUTS_CHANGED);
	HookWidget(ui->advOutTrack4Name,     EDIT_CHANGED,   OUTPUTS_CHANGED);
	HookWidget(ui->channelSetup,         COMBO_CHANGED,  AUDIO_RESTART);
	HookWidget(ui->sampleRate,           COMBO_CHANGED,  AUDIO_RESTART);
	HookWidget(ui->desktopAudioDevice1,  COMBO_CHANGED,  AUDIO_CHANGED);
	HookWidget(ui->desktopAudioDevice2,  COMBO_CHANGED,  AUDIO_CHANGED);
	HookWidget(ui->auxAudioDevice1,      COMBO_CHANGED,  AUDIO_CHANGED);
	HookWidget(ui->auxAudioDevice2,      COMBO_CHANGED,  AUDIO_CHANGED);
	HookWidget(ui->auxAudioDevice3,      COMBO_CHANGED,  AUDIO_CHANGED);
	HookWidget(ui->baseResolution,       CBEDIT_CHANGED, VIDEO_RES);
	HookWidget(ui->outputResolution,     CBEDIT_CHANGED, VIDEO_RES);
	HookWidget(ui->downscaleFilter,      COMBO_CHANGED,  VIDEO_CHANGED);
	HookWidget(ui->fpsType,              COMBO_CHANGED,  VIDEO_CHANGED);
	HookWidget(ui->fpsCommon,            COMBO_CHANGED,  VIDEO_CHANGED);
	HookWidget(ui->fpsInteger,           SCROLL_CHANGED, VIDEO_CHANGED);
	HookWidget(ui->fpsInteger,           SCROLL_CHANGED, VIDEO_CHANGED);
	HookWidget(ui->fpsNumerator,         SCROLL_CHANGED, VIDEO_CHANGED);
	HookWidget(ui->fpsDenominator,       SCROLL_CHANGED, VIDEO_CHANGED);
	HookWidget(ui->renderer,             COMBO_CHANGED,  ADV_RESTART);
	HookWidget(ui->adapter,              COMBO_CHANGED,  ADV_RESTART);
	HookWidget(ui->colorFormat,          COMBO_CHANGED,  ADV_CHANGED);
	HookWidget(ui->colorSpace,           COMBO_CHANGED,  ADV_CHANGED);
	HookWidget(ui->colorRange,           COMBO_CHANGED,  ADV_CHANGED);
	HookWidget(ui->disableOSXVSync,      CHECK_CHANGED,  ADV_CHANGED);
	HookWidget(ui->resetOSXVSync,        CHECK_CHANGED,  ADV_CHANGED);
	HookWidget(ui->streamDelayEnable,    CHECK_CHANGED,  ADV_CHANGED);
	HookWidget(ui->streamDelaySec,       SCROLL_CHANGED, ADV_CHANGED);
	HookWidget(ui->streamDelayPreserve,  CHECK_CHANGED,  ADV_CHANGED);
	HookWidget(ui->reconnectEnable,      CHECK_CHANGED,  ADV_CHANGED);
	HookWidget(ui->reconnectRetryDelay,  SCROLL_CHANGED, ADV_CHANGED);
	HookWidget(ui->reconnectMaxRetries,  SCROLL_CHANGED, ADV_CHANGED);

	// FTL hooks
	HookWidget(ui->advOutFTLStreamKey,	 EDIT_CHANGED,   OUTPUTS_CHANGED);

#ifdef _WIN32
	uint32_t winVer = GetWindowsVersion();
	if (winVer > 0 && winVer < 0x602) {
		toggleAero = new QCheckBox(
				QTStr("Basic.Settings.Video.DisableAero"),
				this);
		QFormLayout *videoLayout =
			reinterpret_cast<QFormLayout*>(ui->videoPage->layout());
		videoLayout->addRow(nullptr, toggleAero);

		HookWidget(toggleAero, CHECK_CHANGED, VIDEO_CHANGED);
		connect(toggleAero, &QAbstractButton::toggled,
				this, &OBSBasicSettings::ToggleDisableAero);
	}
#else
	delete ui->rendererLabel;
	delete ui->renderer;
	delete ui->adapterLabel;
	delete ui->adapter;
	ui->rendererLabel = nullptr;
	ui->renderer = nullptr;
	ui->adapterLabel = nullptr;
	ui->adapter = nullptr;
#endif

#ifndef __APPLE__
	delete ui->disableOSXVSync;
	delete ui->resetOSXVSync;
	ui->disableOSXVSync = nullptr;
	ui->resetOSXVSync = nullptr;
#endif

	connect(ui->streamDelaySec, SIGNAL(valueChanged(int)),
			this, SLOT(UpdateStreamDelayEstimate()));
	connect(ui->advOutTrack1Bitrate, SIGNAL(currentIndexChanged(int)),
			this, SLOT(UpdateStreamDelayEstimate()));
	connect(ui->advOutTrack2Bitrate, SIGNAL(currentIndexChanged(int)),
			this, SLOT(UpdateStreamDelayEstimate()));
	connect(ui->advOutTrack3Bitrate, SIGNAL(currentIndexChanged(int)),
			this, SLOT(UpdateStreamDelayEstimate()));
	connect(ui->advOutTrack4Bitrate, SIGNAL(currentIndexChanged(int)),
			this, SLOT(UpdateStreamDelayEstimate()));

	//Apply button disabled until change.
	EnableApplyButton(false);

	// Load the ingest LoadIngestLocations
	LoadIngestLocations();

	// Initialize libff library
	ff_init();

	installEventFilter(CreateShortcutFilter());

	LoadServiceTypes();
	LoadEncoderTypes();
	LoadColorRanges();
	LoadFormats();

	auto ReloadAudioSources = [](void *data, calldata_t *param)
	{
		auto settings = static_cast<OBSBasicSettings*>(data);
		auto source   = static_cast<obs_source_t*>(calldata_ptr(param,
					"source"));

		if (!source)
			return;

		if (!(obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO))
			return;

		QMetaObject::invokeMethod(settings, "ReloadAudioSources",
				Qt::QueuedConnection);
	};
	sourceCreated.Connect(obs_get_signal_handler(), "source_create",
			ReloadAudioSources, this);
	channelChanged.Connect(obs_get_signal_handler(), "channel_change",
			ReloadAudioSources, this);

	auto ReloadHotkeys = [](void *data, calldata_t*)
	{
		auto settings = static_cast<OBSBasicSettings*>(data);
		QMetaObject::invokeMethod(settings, "ReloadHotkeys");
	};
	hotkeyRegistered.Connect(obs_get_signal_handler(), "hotkey_register",
			ReloadHotkeys, this);

	auto ReloadHotkeysIgnore = [](void *data, calldata_t *param)
	{
		auto settings = static_cast<OBSBasicSettings*>(data);
		auto key      = static_cast<obs_hotkey_t*>(
					calldata_ptr(param,"key"));
		QMetaObject::invokeMethod(settings, "ReloadHotkeys",
				Q_ARG(obs_hotkey_id, obs_hotkey_get_id(key)));
	};
	hotkeyUnregistered.Connect(obs_get_signal_handler(),
			"hotkey_unregister", ReloadHotkeysIgnore, this);

	FillSimpleRecordingValues();

	LoadSettings(false);

	// Add warning checks to advanced output recording section controls
	AdvOutRecCheckWarnings();

	SimpleRecordingQualityChanged();
}

void OBSBasicSettings::SaveCombo(QComboBox *widget, const char *section,
		const char *value)
{
	if (WidgetChanged(widget))
		config_set_string(main->Config(), section, value,
				QT_TO_UTF8(widget->currentText()));
}

void OBSBasicSettings::SaveComboData(QComboBox *widget, const char *section,
		const char *value)
{
	if (WidgetChanged(widget)) {
		QString str = GetComboData(widget);
		config_set_string(main->Config(), section, value,
				QT_TO_UTF8(str));
	}
}

void OBSBasicSettings::SaveCheckBox(QAbstractButton *widget,
		const char *section, const char *value, bool invert)
{
	if (WidgetChanged(widget)) {
		bool checked = widget->isChecked();
		if (invert) checked = !checked;

		config_set_bool(main->Config(), section, value, checked);
	}
}

void OBSBasicSettings::SaveEdit(QLineEdit *widget, const char *section,
		const char *value)
{
	if (WidgetChanged(widget))
		config_set_string(main->Config(), section, value,
				QT_TO_UTF8(widget->text()));
}

void OBSBasicSettings::SaveSpinBox(QSpinBox *widget, const char *section,
		const char *value)
{
	if (WidgetChanged(widget))
		config_set_int(main->Config(), section, value, widget->value());
}

void OBSBasicSettings::LoadServiceTypes()
{
	return;
}

#define TEXT_USE_STREAM_ENC \
	QTStr("Basic.Settings.Output.Adv.Recording.UseStreamEncoder")

void OBSBasicSettings::LoadEncoderTypes()
{
}

#define CS_PARTIAL_STR QTStr("Basic.Settings.Advanced.Video.ColorRange.Partial")
#define CS_FULL_STR    QTStr("Basic.Settings.Advanced.Video.ColorRange.Full")

void OBSBasicSettings::LoadColorRanges()
{
	ui->colorRange->addItem(CS_PARTIAL_STR, "Partial");
	ui->colorRange->addItem(CS_FULL_STR, "Full");
}

#define AV_FORMAT_DEFAULT_STR \
	QTStr("Basic.Settings.Output.Adv.FFmpeg.FormatDefault")
#define AUDIO_STR \
	QTStr("Basic.Settings.Output.Adv.FFmpeg.FormatAudio")
#define VIDEO_STR \
	QTStr("Basic.Settings.Output.Adv.FFmpeg.FormatVideo")

void OBSBasicSettings::LoadFormats()
{
}

void OBSBasicSettings::LoadIngestLocations() {
	ui->advOutFTLIngestLoc->clear();
	ui->advOutFTLIngestLoc->addItem("Australia (Melborne, Victoria)", QString("ingest-sjc.beam.pro"));
	ui->advOutFTLIngestLoc->addItem("Brazil (San Paulo)", QString("ingest-tor.beam.pro"));
	ui->advOutFTLIngestLoc->addItem("Canada (Toronto, ON)", QString("ingest-tor.beam.pro"));
	ui->advOutFTLIngestLoc->addItem("Europe (Amsterdam, Neterlands)", QString("ingest-ams.beam.pro"));
	ui->advOutFTLIngestLoc->addItem("Europe (London, United Kingdom)", QString("ingest-lon.beam.pro"));
	ui->advOutFTLIngestLoc->addItem("Europe (France)", QString("ingest-fra.beam.pro"));
	ui->advOutFTLIngestLoc->addItem("United States (Dallas, TX)", QString("ingest-dal.beam.pro"));
	ui->advOutFTLIngestLoc->addItem("United States (San Jose, CA)", QString("ingest-sjc.beam.pro"));
	ui->advOutFTLIngestLoc->addItem("United States (Seattle, WA)", QString("ingest-sea.beam.pro"));
	ui->advOutFTLIngestLoc->addItem("United States (Washington, DC)", QString 	("ingest-wdc.beam.pro"));

	ui->advOutFTLIngestLoc->insertSeparator(100); // index of 100 forces it to the end
	ui->advOutFTLIngestLoc->addItem("Other", QString(""));
}
static void AddCodec(QComboBox *combo, const ff_codec_desc *codec_desc)
{
	QString itemText(ff_codec_desc_name(codec_desc));
	if (ff_codec_desc_is_alias(codec_desc))
		itemText += QString(" (%1)").arg(
				ff_codec_desc_base_name(codec_desc));

	CodecDesc cd(ff_codec_desc_name(codec_desc),
			ff_codec_desc_id(codec_desc));

	combo->addItem(itemText, qVariantFromValue(cd));
}

#define AV_ENCODER_DEFAULT_STR \
	QTStr("Basic.Settings.Output.Adv.FFmpeg.AVEncoderDefault")

static void AddDefaultCodec(QComboBox *combo, const ff_format_desc *formatDesc,
		ff_codec_type codecType)
{
	CodecDesc cd = GetDefaultCodecDesc(formatDesc, codecType);

	int existingIdx = FindEncoder(combo, cd.name, cd.id);
	if (existingIdx >= 0)
		combo->removeItem(existingIdx);

	combo->addItem(QString("%1 (%2)").arg(cd.name, AV_ENCODER_DEFAULT_STR),
			qVariantFromValue(cd));
}

#define AV_ENCODER_DISABLE_STR \
	QTStr("Basic.Settings.Output.Adv.FFmpeg.AVEncoderDisable")

void OBSBasicSettings::ReloadCodecs(const ff_format_desc *formatDesc)
{
	UNUSED_PARAMETER(formatDesc);
}

void OBSBasicSettings::LoadLanguageList()
{
	const char *currentLang = App()->GetLocale();

	ui->language->clear();

	for (const auto &locale : GetLocaleNames()) {
		int idx = ui->language->count();

		ui->language->addItem(QT_UTF8(locale.second.c_str()),
				QT_UTF8(locale.first.c_str()));

		if (locale.first == currentLang)
			ui->language->setCurrentIndex(idx);
	}

	ui->language->model()->sort(0);
}

void OBSBasicSettings::LoadThemeList()
{
	/* Save theme if user presses Cancel */
	savedTheme = string(App()->GetTheme());

	ui->theme->clear();
	QSet<QString> uniqueSet;
	string themeDir;
	char userThemeDir[512];
	int ret = GetConfigPath(userThemeDir, sizeof(userThemeDir),
			"obs-studio/themes/");
	GetDataFilePath("themes/", themeDir);

	/* Check user dir first. */
	if (ret > 0) {
		QDirIterator it(QString(userThemeDir), QStringList() << "*.qss",
				QDir::Files);
		while (it.hasNext()) {
			it.next();
			QString name = it.fileName().section(".",0,0);
			ui->theme->addItem(name);
			uniqueSet.insert(name);
		}
	}

	/* Check shipped themes. */
	QDirIterator uIt(QString(themeDir.c_str()), QStringList() << "*.qss",
			QDir::Files);
	while (uIt.hasNext()) {
		uIt.next();
		QString name = uIt.fileName().section(".",0,0);
		if (!uniqueSet.contains(name))
			ui->theme->addItem(name);
	}

	int idx = ui->theme->findText(App()->GetTheme());
	if (idx != -1)
		ui->theme->setCurrentIndex(idx);
}

void OBSBasicSettings::LoadGeneralSettings()
{
	loading = true;

	LoadLanguageList();
	LoadThemeList();

	bool warnBeforeStreamStart = config_get_bool(GetGlobalConfig(),
			"BasicWindow", "WarnBeforeStartingStream");
	ui->warnBeforeStreamStart->setChecked(warnBeforeStreamStart);

	bool warnBeforeStreamStop = config_get_bool(GetGlobalConfig(),
			"BasicWindow", "WarnBeforeStoppingStream");
	ui->warnBeforeStreamStop->setChecked(warnBeforeStreamStop);

	loading = false;
}

void OBSBasicSettings::LoadStream1Settings()
{
	return;
}

void OBSBasicSettings::LoadRendererList()
{
#ifdef _WIN32
	const char *renderer = config_get_string(GetGlobalConfig(), "Video",
			"Renderer");

	ui->renderer->addItem(QT_UTF8("Direct3D 11"));
	ui->renderer->addItem(QT_UTF8("OpenGL"));

	int idx = ui->renderer->findText(QT_UTF8(renderer));
	if (idx == -1)
		idx = 0;

	if (strcmp(renderer, "OpenGL") == 0) {
		delete ui->adapter;
		delete ui->adapterLabel;
		ui->adapter = nullptr;
		ui->adapterLabel = nullptr;
	}

	ui->renderer->setCurrentIndex(idx);
#endif
}

Q_DECLARE_METATYPE(MonitorInfo);

static string ResString(uint32_t cx, uint32_t cy)
{
	stringstream res;
	res << cx << "x" << cy;
	return res.str();
}

/* some nice default output resolution vals */
static const double vals[] =
{
	1.0,
	1.25,
	(1.0/0.75),
	1.5,
	(1.0/0.6),
	1.75,
	2.0,
	2.25,
	2.5,
	2.75,
	3.0
};

static const size_t numVals = sizeof(vals)/sizeof(double);

void OBSBasicSettings::ResetDownscales(uint32_t cx, uint32_t cy)
{
	QString advRescale;
	QString advRecRescale;
	QString advFFRescale;
	QString oldOutputRes;
	string bestScale;
	int bestPixelDiff = 0x7FFFFFFF;
	uint32_t out_cx = outputCX;
	uint32_t out_cy = outputCY;

	advFFRescale = ui->advOutFFRescale->lineEdit()->text();

	ui->outputResolution->blockSignals(true);

	ui->outputResolution->clear();
	ui->advOutFFRescale->clear();

	if (!out_cx || !out_cy) {
		out_cx = cx;
		out_cy = cy;
		oldOutputRes = ui->baseResolution->lineEdit()->text();
	} else {
		oldOutputRes = QString::number(out_cx) + "x" +
			QString::number(out_cy);
	}

	for (size_t idx = 0; idx < numVals; idx++) {
		uint32_t downscaleCX = uint32_t(double(cx) / vals[idx]);
		uint32_t downscaleCY = uint32_t(double(cy) / vals[idx]);
		uint32_t outDownscaleCX = uint32_t(double(out_cx) / vals[idx]);
		uint32_t outDownscaleCY = uint32_t(double(out_cy) / vals[idx]);

		downscaleCX &= 0xFFFFFFFC;
		downscaleCY &= 0xFFFFFFFE;
		outDownscaleCX &= 0xFFFFFFFE;
		outDownscaleCY &= 0xFFFFFFFE;

		string res = ResString(downscaleCX, downscaleCY);
		string outRes = ResString(outDownscaleCX, outDownscaleCY);
		ui->outputResolution->addItem(res.c_str());
		ui->advOutFFRescale->addItem(outRes.c_str());

		/* always try to find the closest output resolution to the
		 * previously set output resolution */
		int newPixelCount = int(downscaleCX * downscaleCY);
		int oldPixelCount = int(out_cx * out_cy);
		int diff = abs(newPixelCount - oldPixelCount);

		if (diff < bestPixelDiff) {
			bestScale = res;
			bestPixelDiff = diff;
		}
	}

	string res = ResString(cx, cy);

	float baseAspect   = float(cx) / float(cy);
	float outputAspect = float(out_cx) / float(out_cy);

	bool closeAspect = close_float(baseAspect, outputAspect, 0.01f);
	if (closeAspect)
		ui->outputResolution->lineEdit()->setText(oldOutputRes);
	else
		ui->outputResolution->lineEdit()->setText(bestScale.c_str());

	ui->outputResolution->blockSignals(false);

	if (!closeAspect) {
		ui->outputResolution->setProperty("changed", QVariant(true));
		videoChanged = true;
	}

	if (advRescale.isEmpty())
		advRescale = res.c_str();
	if (advRecRescale.isEmpty())
		advRecRescale = res.c_str();
	if (advFFRescale.isEmpty())
		advFFRescale = res.c_str();

	ui->advOutFFRescale->lineEdit()->setText(advFFRescale);
}

void OBSBasicSettings::LoadDownscaleFilters()
{
	ui->downscaleFilter->addItem(
			QTStr("Basic.Settings.Video.DownscaleFilter.Bilinear"),
			QT_UTF8("bilinear"));
	ui->downscaleFilter->addItem(
			QTStr("Basic.Settings.Video.DownscaleFilter.Bicubic"),
			QT_UTF8("bicubic"));
	ui->downscaleFilter->addItem(
			QTStr("Basic.Settings.Video.DownscaleFilter.Lanczos"),
			QT_UTF8("lanczos"));

	const char *scaleType = config_get_string(main->Config(),
			"Video", "ScaleType");

	if (astrcmpi(scaleType, "bilinear") == 0)
		ui->downscaleFilter->setCurrentIndex(0);
	else if (astrcmpi(scaleType, "lanczos") == 0)
		ui->downscaleFilter->setCurrentIndex(2);
	else
		ui->downscaleFilter->setCurrentIndex(1);
}

void OBSBasicSettings::LoadResolutionLists()
{
	uint32_t cx = config_get_uint(main->Config(), "Video", "BaseCX");
	uint32_t cy = config_get_uint(main->Config(), "Video", "BaseCY");
	uint32_t out_cx = config_get_uint(main->Config(), "Video", "OutputCX");
	uint32_t out_cy = config_get_uint(main->Config(), "Video", "OutputCY");
	vector<MonitorInfo> monitors;

	ui->baseResolution->clear();

	GetMonitors(monitors);

	for (MonitorInfo &monitor : monitors) {
		string res = ResString(monitor.cx, monitor.cy);
		ui->baseResolution->addItem(res.c_str());
	}

	string outputResString = ResString(out_cx, out_cy);

	ui->baseResolution->lineEdit()->setText(ResString(cx, cy).c_str());

	RecalcOutputResPixels(outputResString.c_str());
	ResetDownscales(cx, cy);

	ui->outputResolution->lineEdit()->setText(outputResString.c_str());
}

static inline void LoadFPSCommon(OBSBasic *main, Ui::OBSBasicSettings *ui)
{
	const char *val = config_get_string(main->Config(), "Video",
			"FPSCommon");

	int idx = ui->fpsCommon->findText(val);
	if (idx == -1) idx = 3;
	ui->fpsCommon->setCurrentIndex(idx);
}

static inline void LoadFPSInteger(OBSBasic *main, Ui::OBSBasicSettings *ui)
{
	uint32_t val = config_get_uint(main->Config(), "Video", "FPSInt");
	ui->fpsInteger->setValue(val);
}

static inline void LoadFPSFraction(OBSBasic *main, Ui::OBSBasicSettings *ui)
{
	uint32_t num = config_get_uint(main->Config(), "Video", "FPSNum");
	uint32_t den = config_get_uint(main->Config(), "Video", "FPSDen");

	ui->fpsNumerator->setValue(num);
	ui->fpsDenominator->setValue(den);
}

void OBSBasicSettings::LoadFPSData()
{
	LoadFPSCommon(main, ui.get());
	LoadFPSInteger(main, ui.get());
	LoadFPSFraction(main, ui.get());

	uint32_t fpsType = config_get_uint(main->Config(), "Video",
			"FPSType");
	if (fpsType > 2) fpsType = 0;

	ui->fpsType->setCurrentIndex(fpsType);
	ui->fpsTypes->setCurrentIndex(fpsType);
}

void OBSBasicSettings::LoadVideoSettings()
{
	loading = true;

	if (video_output_active(obs_get_video())) {
		ui->videoPage->setEnabled(false);
		ui->videoMsg->setText(
				QTStr("Basic.Settings.Video.CurrentlyActive"));
	}

	LoadResolutionLists();
	LoadFPSData();
	LoadDownscaleFilters();

#ifdef _WIN32
	if (toggleAero) {
		bool disableAero = config_get_bool(main->Config(), "Video",
				"DisableAero");
		toggleAero->setChecked(disableAero);

		aeroWasDisabled = disableAero;
	}
#endif

	loading = false;
}

void OBSBasicSettings::LoadSimpleOutputSettings()
{
}

void OBSBasicSettings::LoadAdvOutputStreamingSettings()
{
}

OBSPropertiesView *OBSBasicSettings::CreateEncoderPropertyView(
		const char *encoder, const char *path, bool changed)
{
	obs_data_t *settings = obs_encoder_defaults(encoder);
	OBSPropertiesView *view;

	char encoderJsonPath[512];
	int ret = GetProfilePath(encoderJsonPath, sizeof(encoderJsonPath),
			path);
	if (ret > 0) {
		obs_data_t *data = obs_data_create_from_json_file_safe(
				encoderJsonPath, "bak");
		obs_data_apply(settings, data);
		obs_data_release(data);
	}

	view = new OBSPropertiesView(settings, encoder,
			(PropertiesReloadCallback)obs_get_encoder_properties,
			170);
	view->setFrameShape(QFrame::StyledPanel);
	view->setProperty("changed", QVariant(changed));
	QObject::connect(view, SIGNAL(Changed()), this, SLOT(OutputsChanged()));

	obs_data_release(settings);
	return view;
}

void OBSBasicSettings::LoadAdvOutputStreamingEncoderProperties()
{
}

void OBSBasicSettings::LoadAdvOutputRecordingSettings()
{
}

void OBSBasicSettings::LoadAdvOutputRecordingEncoderProperties()
{
}

static void SelectFormat(QComboBox *combo, const char *name,
		const char *mimeType)
{
	FormatDesc formatDesc(name, mimeType);

	for(int i = 0; i < combo->count(); i++) {
		QVariant v = combo->itemData(i);
		if (!v.isNull()) {
			if (formatDesc == v.value<FormatDesc>()) {
				combo->setCurrentIndex(i);
				return;
			}
		}
	}

	combo->setCurrentIndex(0);
}

static void SelectEncoder(QComboBox *combo, const char *name, int id)
{
	int idx = FindEncoder(combo, name, id);
	if (idx >= 0)
		combo->setCurrentIndex(idx);
}

void OBSBasicSettings::LoadAdvOutputFFmpegSettings()
{
	const char *url = config_get_string(main->Config(), "AdvOut", "FFURL");
	int videoBitrate = config_get_int(main->Config(), "AdvOut",
			"FFVBitrate");
	bool rescale = config_get_bool(main->Config(), "AdvOut",
			"FFRescale");
	const char *rescaleRes = config_get_string(main->Config(), "AdvOut",
			"FFRescaleRes");
	int audioBitrate = config_get_int(main->Config(), "AdvOut",
			"FFABitrate");
	int audioTrack = config_get_int(main->Config(), "AdvOut",
			"FFAudioTrack");

	int ftlChannelId = config_get_int(main->Config(), "AdvOut",
			"FTLChannelID");
	const char *ftlStreamKey = config_get_string(main->Config(), "AdvOut",
			"FTLStreamKey");

	/* Set the dropdown on ingest correctly based on saved settings */
	int known_ingests = ui->advOutFTLIngestLoc->count();
	QString saved_ingest(url);

	bool match_found = false;
	for (int i = 0; i != known_ingests; i++) {
		QString ingest_url;
		ingest_url = ui->advOutFTLIngestLoc->itemData(i).toString();

		blog (LOG_INFO, "test %s %s", ingest_url.toStdString().c_str(), saved_ingest.toStdString().c_str());
		// See if this ingest matches the current index
		if (ingest_url == saved_ingest) {
			// Yaztee!, we've got a match
			ui->advOutFTLIngestLoc->setCurrentIndex(i);
			ui->advOutSavePathURLlabel->hide();
			ui->advOutFFURL->hide();
			match_found = true;
			break;
		}
	}

	if (match_found != true) {
		/* Set the dropdown to custom which is always the bottom option */
		ui->advOutFTLIngestLoc->setCurrentIndex(ui->advOutFTLIngestLoc->count()-1);
	}
	ui->advOutFFURL->setText(QT_UTF8(url));
	ui->advOutFFVBitrate->setValue(videoBitrate);
	ui->advOutFFUseRescale->setChecked(rescale);
	ui->advOutFFRescale->setEnabled(rescale);
	ui->advOutFFRescale->setCurrentText(rescaleRes);
	ui->advOutFFABitrate->setValue(audioBitrate);

	/* Load FTL UI bits */
	ui->advOutFTLStreamKey->setText(QT_UTF8(ftlStreamKey));

	switch (audioTrack) {
	case 1: ui->advOutFFTrack1->setChecked(true); break;
	case 2: ui->advOutFFTrack2->setChecked(true); break;
	case 3: ui->advOutFFTrack3->setChecked(true); break;
	case 4: ui->advOutFFTrack4->setChecked(true); break;
	}
}

void OBSBasicSettings::LoadAdvOutputAudioSettings()
{
	int track1Bitrate = config_get_uint(main->Config(), "AdvOut",
			"Track1Bitrate");
	int track2Bitrate = config_get_uint(main->Config(), "AdvOut",
			"Track2Bitrate");
	int track3Bitrate = config_get_uint(main->Config(), "AdvOut",
			"Track3Bitrate");
	int track4Bitrate = config_get_uint(main->Config(), "AdvOut",
			"Track4Bitrate");
	const char *name1 = config_get_string(main->Config(), "AdvOut",
			"Track1Name");
	const char *name2 = config_get_string(main->Config(), "AdvOut",
			"Track2Name");
	const char *name3 = config_get_string(main->Config(), "AdvOut",
			"Track3Name");
	const char *name4 = config_get_string(main->Config(), "AdvOut",
			"Track4Name");

	track1Bitrate = FindClosestAvailableAACBitrate(track1Bitrate);
	track2Bitrate = FindClosestAvailableAACBitrate(track2Bitrate);
	track3Bitrate = FindClosestAvailableAACBitrate(track3Bitrate);
	track4Bitrate = FindClosestAvailableAACBitrate(track4Bitrate);

	SetComboByName(ui->advOutTrack1Bitrate,
			std::to_string(track1Bitrate).c_str());
	SetComboByName(ui->advOutTrack2Bitrate,
			std::to_string(track2Bitrate).c_str());
	SetComboByName(ui->advOutTrack3Bitrate,
			std::to_string(track3Bitrate).c_str());
	SetComboByName(ui->advOutTrack4Bitrate,
			std::to_string(track4Bitrate).c_str());

	ui->advOutTrack1Name->setText(name1);
	ui->advOutTrack2Name->setText(name2);
	ui->advOutTrack3Name->setText(name3);
	ui->advOutTrack4Name->setText(name4);
}

void OBSBasicSettings::LoadOutputSettings()
{
	loading = true;

	LoadSimpleOutputSettings();
	LoadAdvOutputStreamingSettings();
	LoadAdvOutputStreamingEncoderProperties();
	LoadAdvOutputRecordingSettings();
	LoadAdvOutputRecordingEncoderProperties();
	LoadAdvOutputFFmpegSettings();
	LoadAdvOutputAudioSettings();

	if (video_output_active(obs_get_video())) {
		ui->advOutputAudioTracksTab->setEnabled(false);
	}

	loading = false;
}

void OBSBasicSettings::SetAdvOutputFFmpegEnablement(
		ff_codec_type encoderType, bool enabled,
		bool enableEncoder)
{
	UNUSED_PARAMETER(enableEncoder);
	bool rescale = config_get_bool(main->Config(), "AdvOut",
			"FFRescale");

	switch (encoderType) {
	case FF_CODEC_VIDEO:
		ui->advOutFFVBitrate->setEnabled(enabled);
		ui->advOutFFUseRescale->setEnabled(enabled);
		ui->advOutFFRescale->setEnabled(enabled && rescale);
		break;
	case FF_CODEC_AUDIO:
		ui->advOutFFABitrate->setEnabled(enabled);
		ui->advOutFFTrack1->setEnabled(enabled);
		ui->advOutFFTrack2->setEnabled(enabled);
		ui->advOutFFTrack3->setEnabled(enabled);
		ui->advOutFFTrack4->setEnabled(enabled);
	default:
		break;
	}
}

static inline void LoadListValue(QComboBox *widget, const char *text,
		const char *val)
{
	widget->addItem(QT_UTF8(text), QT_UTF8(val));
}

void OBSBasicSettings::LoadListValues(QComboBox *widget, obs_property_t *prop,
		int index)
{
	size_t count = obs_property_list_item_count(prop);

	obs_source_t *source = obs_get_output_source(index);
	const char *deviceId = nullptr;
	obs_data_t *settings = nullptr;

	if (source) {
		settings = obs_source_get_settings(source);
		if (settings)
			deviceId = obs_data_get_string(settings, "device_id");
	}

	widget->addItem(QTStr("Disabled"), "disabled");

	for (size_t i = 0; i < count; i++) {
		const char *name = obs_property_list_item_name(prop, i);
		const char *val  = obs_property_list_item_string(prop, i);
		LoadListValue(widget, name, val);
	}

	if (deviceId) {
		QVariant var(QT_UTF8(deviceId));
		int idx = widget->findData(var);
		if (idx != -1) {
			widget->setCurrentIndex(idx);
		} else {
			widget->insertItem(0,
					QTStr("Basic.Settings.Audio."
						"UnknownAudioDevice"),
					var);
			widget->setCurrentIndex(0);
		}
	}

	if (settings)
		obs_data_release(settings);
	if (source)
		obs_source_release(source);
}

void OBSBasicSettings::LoadAudioDevices()
{
	const char *input_id  = App()->InputAudioSource();
	const char *output_id = App()->OutputAudioSource();

	obs_properties_t *input_props = obs_get_source_properties(input_id);
	obs_properties_t *output_props = obs_get_source_properties(output_id);

	if (input_props) {
		obs_property_t *inputs  = obs_properties_get(input_props,
				"device_id");
		LoadListValues(ui->auxAudioDevice1, inputs, 3);
		LoadListValues(ui->auxAudioDevice2, inputs, 4);
		LoadListValues(ui->auxAudioDevice3, inputs, 5);
		obs_properties_destroy(input_props);
	}

	if (output_props) {
		obs_property_t *outputs = obs_properties_get(output_props,
				"device_id");
		LoadListValues(ui->desktopAudioDevice1, outputs, 1);
		LoadListValues(ui->desktopAudioDevice2, outputs, 2);
		obs_properties_destroy(output_props);
	}
}

#define NBSP "\xC2\xA0"

void OBSBasicSettings::LoadAudioSources()
{
	auto layout = new QFormLayout();
	layout->setVerticalSpacing(15);
	layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	ui->audioSourceScrollArea->takeWidget()->deleteLater();
	audioSourceSignals.clear();
	audioSources.clear();

	auto widget = new QWidget();
	widget->setLayout(layout);
	ui->audioSourceScrollArea->setWidget(widget);

	const char *enablePtm = Str("Basic.Settings.Audio.EnablePushToMute");
	const char *ptmDelay  = Str("Basic.Settings.Audio.PushToMuteDelay");
	const char *enablePtt = Str("Basic.Settings.Audio.EnablePushToTalk");
	const char *pttDelay  = Str("Basic.Settings.Audio.PushToTalkDelay");
	auto AddSource = [&](obs_source_t *source)
	{
		if (!(obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO))
			return true;

		auto form = new QFormLayout();
		form->setVerticalSpacing(0);
		form->setHorizontalSpacing(5);
		form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

		auto ptmCB = new SilentUpdateCheckBox();
		ptmCB->setText(enablePtm);
		ptmCB->setChecked(obs_source_push_to_mute_enabled(source));
		form->addRow(ptmCB);

		auto ptmSB = new SilentUpdateSpinBox();
		ptmSB->setSuffix(NBSP "ms");
		ptmSB->setRange(0, INT_MAX);
		ptmSB->setValue(obs_source_get_push_to_mute_delay(source));
		form->addRow(ptmDelay, ptmSB);

		auto pttCB = new SilentUpdateCheckBox();
		pttCB->setText(enablePtt);
		pttCB->setChecked(obs_source_push_to_talk_enabled(source));
		form->addRow(pttCB);

		auto pttSB = new SilentUpdateSpinBox();
		pttSB->setSuffix(NBSP "ms");
		pttSB->setRange(0, INT_MAX);
		pttSB->setValue(obs_source_get_push_to_talk_delay(source));
		form->addRow(pttDelay, pttSB);

		HookWidget(ptmCB, CHECK_CHANGED,  AUDIO_CHANGED);
		HookWidget(ptmSB, SCROLL_CHANGED, AUDIO_CHANGED);
		HookWidget(pttCB, CHECK_CHANGED,  AUDIO_CHANGED);
		HookWidget(pttSB, SCROLL_CHANGED, AUDIO_CHANGED);

		audioSourceSignals.reserve(audioSourceSignals.size() + 4);

		auto handler = obs_source_get_signal_handler(source);
		audioSourceSignals.emplace_back(handler, "push_to_mute_changed",
				[](void *data, calldata_t *param)
		{
			QMetaObject::invokeMethod(static_cast<QObject*>(data),
				"setCheckedSilently",
				Q_ARG(bool, calldata_bool(param, "enabled")));
		}, ptmCB);
		audioSourceSignals.emplace_back(handler, "push_to_mute_delay",
				[](void *data, calldata_t *param)
		{
			QMetaObject::invokeMethod(static_cast<QObject*>(data),
				"setValueSilently",
				Q_ARG(int, calldata_int(param, "delay")));
		}, ptmSB);
		audioSourceSignals.emplace_back(handler, "push_to_talk_changed",
				[](void *data, calldata_t *param)
		{
			QMetaObject::invokeMethod(static_cast<QObject*>(data),
				"setCheckedSilently",
				Q_ARG(bool, calldata_bool(param, "enabled")));
		}, pttCB);
		audioSourceSignals.emplace_back(handler, "push_to_talk_delay",
				[](void *data, calldata_t *param)
		{
			QMetaObject::invokeMethod(static_cast<QObject*>(data),
				"setValueSilently",
				Q_ARG(int, calldata_int(param, "delay")));
		}, pttSB);

		audioSources.emplace_back(OBSGetWeakRef(source),
				ptmCB, pttSB, pttCB, pttSB);

		auto label = new OBSSourceLabel(source);
		connect(label, &OBSSourceLabel::Removed,
				[=]()
				{
					LoadAudioSources();
				});
		connect(label, &OBSSourceLabel::Destroyed,
				[=]()
				{
					LoadAudioSources();
				});

		layout->addRow(label, form);
		return true;
	};

	using AddSource_t = decltype(AddSource);
	obs_enum_sources([](void *data, obs_source_t *source)
	{
		auto &AddSource = *static_cast<AddSource_t*>(data);
		AddSource(source);
		return true;
	}, static_cast<void*>(&AddSource));


	if (layout->rowCount() == 0)
		ui->audioSourceScrollArea->hide();
	else
		ui->audioSourceScrollArea->show();
}

void OBSBasicSettings::LoadAudioSettings()
{
	uint32_t sampleRate = config_get_uint(main->Config(), "Audio",
			"SampleRate");
	const char *speakers = config_get_string(main->Config(), "Audio",
			"ChannelSetup");

	loading = true;

	const char *str;
	if (sampleRate == 48000)
		str = "48khz";
	else
		str = "44.1khz";

	int sampleRateIdx = ui->sampleRate->findText(str);
	if (sampleRateIdx != -1)
		ui->sampleRate->setCurrentIndex(sampleRateIdx);

	if (strcmp(speakers, "Mono") == 0)
		ui->channelSetup->setCurrentIndex(0);
	else
		ui->channelSetup->setCurrentIndex(1);

	LoadAudioDevices();
	LoadAudioSources();

	loading = false;
}

void OBSBasicSettings::LoadAdvancedSettings()
{
	const char *videoColorFormat = config_get_string(main->Config(),
			"Video", "ColorFormat");
	const char *videoColorSpace = config_get_string(main->Config(),
			"Video", "ColorSpace");
	const char *videoColorRange = config_get_string(main->Config(),
			"Video", "ColorRange");
	bool enableDelay = config_get_bool(main->Config(), "Output",
			"DelayEnable");
	int delaySec = config_get_int(main->Config(), "Output",
			"DelaySec");
	bool preserveDelay = config_get_bool(main->Config(), "Output",
			"DelayPreserve");
	bool reconnect = config_get_bool(main->Config(), "Output",
			"Reconnect");
	int retryDelay = config_get_int(main->Config(), "Output",
			"RetryDelay");
	int maxRetries = config_get_int(main->Config(), "Output",
			"MaxRetries");

	loading = true;

	LoadRendererList();

	ui->reconnectEnable->setChecked(reconnect);
	ui->reconnectRetryDelay->setValue(retryDelay);
	ui->reconnectMaxRetries->setValue(maxRetries);

	ui->streamDelaySec->setValue(delaySec);
	ui->streamDelayPreserve->setChecked(preserveDelay);
	ui->streamDelayEnable->setChecked(enableDelay);

	SetComboByName(ui->colorFormat, videoColorFormat);
	SetComboByName(ui->colorSpace, videoColorSpace);
	SetComboByValue(ui->colorRange, videoColorRange);

	if (video_output_active(obs_get_video())) {
		ui->advancedVideoContainer->setEnabled(false);
	}

#ifdef __APPLE__
	bool disableOSXVSync = config_get_bool(App()->GlobalConfig(),
			"Video", "DisableOSXVSync");
	bool resetOSXVSync = config_get_bool(App()->GlobalConfig(),
			"Video", "ResetOSXVSyncOnExit");
	ui->disableOSXVSync->setChecked(disableOSXVSync);
	ui->resetOSXVSync->setChecked(resetOSXVSync);
	ui->resetOSXVSync->setEnabled(disableOSXVSync);
#endif

	loading = false;
}

template <typename Func>
static inline void LayoutHotkey(obs_hotkey_id id, obs_hotkey_t *key, Func &&fun,
		const map<obs_hotkey_id, vector<obs_key_combination_t>> &keys)
{
	auto *label = new OBSHotkeyLabel;
	label->setText(obs_hotkey_get_description(key));

	OBSHotkeyWidget *hw = nullptr;

	auto combos = keys.find(id);
	if (combos == std::end(keys))
		hw = new OBSHotkeyWidget(id, obs_hotkey_get_name(key));
	else
		hw = new OBSHotkeyWidget(id, obs_hotkey_get_name(key),
				combos->second);

	hw->label = label;
	label->widget = hw;

	fun(key, label, hw);
}

template <typename Func, typename T>
static QLabel *makeLabel(T &t, Func &&getName)
{
	return new QLabel(getName(t));
}

template <typename Func>
static QLabel *makeLabel(const OBSSource &source, Func &&)
{
	return new OBSSourceLabel(source);
}

template <typename Func, typename T>
static inline void AddHotkeys(QFormLayout &layout,
		Func &&getName, std::vector<
			std::tuple<T, QPointer<QLabel>, QPointer<QWidget>>
		> &hotkeys)
{
	if (hotkeys.empty())
		return;

	auto line = new QFrame();
	line->setFrameShape(QFrame::HLine);
	line->setFrameShadow(QFrame::Sunken);

	layout.setItem(layout.rowCount(), QFormLayout::SpanningRole,
			new QSpacerItem(0, 10));
	layout.addRow(line);

	using tuple_type =
		std::tuple<T, QPointer<QLabel>, QPointer<QWidget>>;

	stable_sort(begin(hotkeys), end(hotkeys),
			[&](const tuple_type &a, const tuple_type &b)
	{
		const auto &o_a = get<0>(a);
		const auto &o_b = get<0>(b);
		return o_a != o_b &&
			string(getName(o_a)) <
				getName(o_b);
	});

	string prevName;
	for (const auto &hotkey : hotkeys) {
		const auto &o = get<0>(hotkey);
		const char *name = getName(o);
		if (prevName != name) {
			prevName = name;
			layout.setItem(layout.rowCount(),
					QFormLayout::SpanningRole,
					new QSpacerItem(0, 10));
			layout.addRow(makeLabel(o, getName));
		}

		auto hlabel = get<1>(hotkey);
		auto widget = get<2>(hotkey);
		layout.addRow(hlabel, widget);
	}
}

void OBSBasicSettings::LoadHotkeySettings(obs_hotkey_id ignoreKey)
{
	hotkeys.clear();
	ui->hotkeyPage->takeWidget()->deleteLater();

	using keys_t = map<obs_hotkey_id, vector<obs_key_combination_t>>;
	keys_t keys;
	obs_enum_hotkey_bindings([](void *data,
				size_t, obs_hotkey_binding_t *binding)
	{
		auto &keys = *static_cast<keys_t*>(data);

		keys[obs_hotkey_binding_get_hotkey_id(binding)].emplace_back(
			obs_hotkey_binding_get_key_combination(binding));

		return true;
	}, &keys);

	auto layout = new QFormLayout();
	layout->setVerticalSpacing(0);
	layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	layout->setLabelAlignment(
			Qt::AlignRight|Qt::AlignTrailing|Qt::AlignVCenter);

	auto widget = new QWidget();
	widget->setLayout(layout);
	ui->hotkeyPage->setWidget(widget);

	using namespace std;
	using encoders_elem_t =
		tuple<OBSEncoder, QPointer<QLabel>, QPointer<QWidget>>;
	using outputs_elem_t =
		tuple<OBSOutput, QPointer<QLabel>, QPointer<QWidget>>;
	using services_elem_t =
		tuple<OBSService, QPointer<QLabel>, QPointer<QWidget>>;
	using sources_elem_t =
		tuple<OBSSource, QPointer<QLabel>, QPointer<QWidget>>;
	vector<encoders_elem_t> encoders;
	vector<outputs_elem_t>  outputs;
	vector<services_elem_t> services;
	vector<sources_elem_t>  scenes;
	vector<sources_elem_t>  sources;

	vector<obs_hotkey_id> pairIds;
	map<obs_hotkey_id, pair<obs_hotkey_id, OBSHotkeyLabel*>> pairLabels;

	using std::move;

	auto HandleEncoder = [&](void *registerer, OBSHotkeyLabel *label,
			OBSHotkeyWidget *hw)
	{
		auto weak_encoder =
			static_cast<obs_weak_encoder_t*>(registerer);
		auto encoder = OBSGetStrongRef(weak_encoder);

		if (!encoder)
			return true;

		encoders.emplace_back(move(encoder), label, hw);
		return false;
	};

	auto HandleOutput = [&](void *registerer, OBSHotkeyLabel *label,
			OBSHotkeyWidget *hw)
	{
		auto weak_output = static_cast<obs_weak_output_t*>(registerer);
		auto output = OBSGetStrongRef(weak_output);

		if (!output)
			return true;

		outputs.emplace_back(move(output), label, hw);
		return false;
	};

	auto HandleService = [&](void *registerer, OBSHotkeyLabel *label,
			OBSHotkeyWidget *hw)
	{
		auto weak_service =
			static_cast<obs_weak_service_t*>(registerer);
		auto service = OBSGetStrongRef(weak_service);

		if (!service)
			return true;

		services.emplace_back(move(service), label, hw);
		return false;
	};

	auto HandleSource = [&](void *registerer, OBSHotkeyLabel *label,
			OBSHotkeyWidget *hw)
	{
		auto weak_source = static_cast<obs_weak_source_t*>(registerer);
		auto source = OBSGetStrongRef(weak_source);

		if (!source)
			return true;

		if (obs_scene_from_source(source))
			scenes.emplace_back(source, label, hw);
		else
			sources.emplace_back(source, label, hw);

		return false;
	};

	auto RegisterHotkey = [&](obs_hotkey_t *key, OBSHotkeyLabel *label,
			OBSHotkeyWidget *hw)
	{
		auto registerer_type = obs_hotkey_get_registerer_type(key);
		void *registerer     = obs_hotkey_get_registerer(key);

		obs_hotkey_id partner = obs_hotkey_get_pair_partner_id(key);
		if (partner != OBS_INVALID_HOTKEY_ID) {
			pairLabels.emplace(obs_hotkey_get_id(key),
					make_pair(partner, label));
			pairIds.push_back(obs_hotkey_get_id(key));
		}

		using std::move;

		switch (registerer_type) {
		case OBS_HOTKEY_REGISTERER_FRONTEND:
			layout->addRow(label, hw);
			break;

		case OBS_HOTKEY_REGISTERER_ENCODER:
			if (HandleEncoder(registerer, label, hw))
				return;
			break;

		case OBS_HOTKEY_REGISTERER_OUTPUT:
			if (HandleOutput(registerer, label, hw))
				return;
			break;

		case OBS_HOTKEY_REGISTERER_SERVICE:
			if (HandleService(registerer, label, hw))
				return;
			break;

		case OBS_HOTKEY_REGISTERER_SOURCE:
			if (HandleSource(registerer, label, hw))
				return;
			break;
		}

		hotkeys.emplace_back(registerer_type ==
				OBS_HOTKEY_REGISTERER_FRONTEND, hw);
		connect(hw, &OBSHotkeyWidget::KeyChanged,
				this, &OBSBasicSettings::HotkeysChanged);
	};

	auto data = make_tuple(RegisterHotkey, std::move(keys), ignoreKey);
	using data_t = decltype(data);
	obs_enum_hotkeys([](void *data, obs_hotkey_id id, obs_hotkey_t *key)
	{
		data_t &d = *static_cast<data_t*>(data);
		if (id != get<2>(d))
			LayoutHotkey(id, key, get<0>(d), get<1>(d));
		return true;
	}, &data);

	for (auto keyId : pairIds) {
		auto data1 = pairLabels.find(keyId);
		if (data1 == end(pairLabels))
			continue;

		auto &label1 = data1->second.second;
		if (label1->pairPartner)
			continue;

		auto data2 = pairLabels.find(data1->second.first);
		if (data2 == end(pairLabels))
			continue;

		auto &label2 = data2->second.second;
		if (label2->pairPartner)
			continue;

		QString tt = QTStr("Basic.Settings.Hotkeys.Pair");
		auto name1 = label1->text();
		auto name2 = label2->text();

		auto Update = [&](OBSHotkeyLabel *label, const QString &name,
				OBSHotkeyLabel *other, const QString &otherName)
		{
			label->setToolTip(tt.arg(otherName));
			label->setText(name + " *");
			label->pairPartner = other;
		};
		Update(label1, name1, label2, name2);
		Update(label2, name2, label1, name1);
	}

	AddHotkeys(*layout, obs_output_get_name, outputs);
	AddHotkeys(*layout, obs_source_get_name, scenes);
	AddHotkeys(*layout, obs_source_get_name, sources);
	AddHotkeys(*layout, obs_encoder_get_name, encoders);
	AddHotkeys(*layout, obs_service_get_name, services);
}

void OBSBasicSettings::LoadSettings(bool changedOnly)
{
	if (!changedOnly || generalChanged)
		LoadGeneralSettings();
	if (!changedOnly || stream1Changed)
		LoadStream1Settings();
	if (!changedOnly || outputsChanged)
		LoadOutputSettings();
	if (!changedOnly || audioChanged)
		LoadAudioSettings();
	if (!changedOnly || videoChanged)
		LoadVideoSettings();
	if (!changedOnly || hotkeysChanged)
		LoadHotkeySettings();
	if (!changedOnly || advancedChanged)
		LoadAdvancedSettings();
}

void OBSBasicSettings::SaveGeneralSettings()
{
	int languageIndex = ui->language->currentIndex();
	QVariant langData = ui->language->itemData(languageIndex);
	string language = langData.toString().toStdString();

	if (WidgetChanged(ui->language))
		config_set_string(GetGlobalConfig(), "General", "Language",
				language.c_str());

	int themeIndex = ui->theme->currentIndex();
	QString themeData = ui->theme->itemText(themeIndex);
	string theme = themeData.toStdString();

	if (WidgetChanged(ui->theme)) {
		config_set_string(GetGlobalConfig(), "General", "Theme",
				  theme.c_str());
		App()->SetTheme(theme);
	}

	config_set_bool(GetGlobalConfig(), "BasicWindow",
			"WarnBeforeStartingStream",
			ui->warnBeforeStreamStart->isChecked());
	config_set_bool(GetGlobalConfig(), "BasicWindow",
			"WarnBeforeStoppingStream",
			ui->warnBeforeStreamStop->isChecked());
}

void OBSBasicSettings::SaveStream1Settings()
{
	return;
}

void OBSBasicSettings::SaveVideoSettings()
{
	QString baseResolution   = ui->baseResolution->currentText();
	QString outputResolution = ui->outputResolution->currentText();
	int     fpsType          = ui->fpsType->currentIndex();
	uint32_t cx = 0, cy = 0;

	/* ------------------- */

	if (WidgetChanged(ui->baseResolution) &&
	    ConvertResText(QT_TO_UTF8(baseResolution), cx, cy)) {
		config_set_uint(main->Config(), "Video", "BaseCX", cx);
		config_set_uint(main->Config(), "Video", "BaseCY", cy);
	}

	if (WidgetChanged(ui->outputResolution) &&
	    ConvertResText(QT_TO_UTF8(outputResolution), cx, cy)) {
		config_set_uint(main->Config(), "Video", "OutputCX", cx);
		config_set_uint(main->Config(), "Video", "OutputCY", cy);
	}

	if (WidgetChanged(ui->fpsType))
		config_set_uint(main->Config(), "Video", "FPSType", fpsType);

	SaveCombo(ui->fpsCommon, "Video", "FPSCommon");
	SaveSpinBox(ui->fpsInteger, "Video", "FPSInt");
	SaveSpinBox(ui->fpsNumerator, "Video", "FPSNum");
	SaveSpinBox(ui->fpsDenominator, "Video", "FPSDen");
	SaveComboData(ui->downscaleFilter, "Video", "ScaleType");

#ifdef _WIN32
	if (toggleAero) {
		SaveCheckBox(toggleAero, "Video", "DisableAero");
		aeroWasDisabled = toggleAero->isChecked();
	}
#endif
}

void OBSBasicSettings::SaveAdvancedSettings()
{
#ifdef _WIN32
	if (WidgetChanged(ui->renderer))
		config_set_string(App()->GlobalConfig(), "Video", "Renderer",
				QT_TO_UTF8(ui->renderer->currentText()));
#endif

#ifdef __APPLE__
	if (WidgetChanged(ui->disableOSXVSync)) {
		bool disable = ui->disableOSXVSync->isChecked();
		config_set_bool(App()->GlobalConfig(),
				"Video", "DisableOSXVSync", disable);
		EnableOSXVSync(!disable);
	}
	if (WidgetChanged(ui->resetOSXVSync))
		config_set_bool(App()->GlobalConfig(),
				"Video", "ResetOSXVSyncOnExit",
				ui->resetOSXVSync->isChecked());
#endif

	SaveCombo(ui->colorFormat, "Video", "ColorFormat");
	SaveCombo(ui->colorSpace, "Video", "ColorSpace");
	SaveComboData(ui->colorRange, "Video", "ColorRange");
	SaveCheckBox(ui->streamDelayEnable, "Output", "DelayEnable");
	SaveSpinBox(ui->streamDelaySec, "Output", "DelaySec");
	SaveCheckBox(ui->streamDelayPreserve, "Output", "DelayPreserve");
	SaveCheckBox(ui->reconnectEnable, "Output", "Reconnect");
	SaveSpinBox(ui->reconnectRetryDelay, "Output", "RetryDelay");
	SaveSpinBox(ui->reconnectMaxRetries, "Output", "MaxRetries");
}

static inline const char *OutputModeFromIdx(int idx)
{
	if (idx == 1)
		return "Advanced";
	else
		return "Simple";
}

static inline const char *RecTypeFromIdx(int idx)
{
	if (idx == 1)
		return "FFmpeg";
	else
		return "Standard";
}

static void WriteJsonData(OBSPropertiesView *view, const char *path)
{
	char full_path[512];

	if (!view || !WidgetChanged(view))
		return;

	int ret = GetProfilePath(full_path, sizeof(full_path), path);
	if (ret > 0) {
		obs_data_t *settings = view->GetSettings();
		if (settings) {
			obs_data_save_json_safe(settings, full_path,
					"tmp", "bak");
		}
	}
}

static void SaveTrackIndex(config_t *config, const char *section,
		const char *name,
		QAbstractButton *check1,
		QAbstractButton *check2,
		QAbstractButton *check3,
		QAbstractButton *check4)
{
	if (check1->isChecked()) config_set_int(config, section, name, 1);
	else if (check2->isChecked()) config_set_int(config, section, name, 2);
	else if (check3->isChecked()) config_set_int(config, section, name, 3);
	else if (check4->isChecked()) config_set_int(config, section, name, 4);
}

void OBSBasicSettings::SaveFormat(QComboBox *combo)
{
	QVariant v = combo->currentData();
	if (!v.isNull()) {
		FormatDesc desc = v.value<FormatDesc>();
		config_set_string(main->Config(), "AdvOut", "FFFormat",
				desc.name);
		config_set_string(main->Config(), "AdvOut", "FFFormatMimeType",
				desc.mimeType);

		const char *ext = ff_format_desc_extensions(desc.desc);
		string extStr = ext ? ext : "";

		char *comma = strchr(&extStr[0], ',');
		if (comma)
			comma = 0;

		config_set_string(main->Config(), "AdvOut", "FFExtension",
				extStr.c_str());
	} else {
		config_set_string(main->Config(), "AdvOut", "FFFormat",
				nullptr);
		config_set_string(main->Config(), "AdvOut", "FFFormatMimeType",
				nullptr);

		config_remove_value(main->Config(), "AdvOut", "FFExtension");
	}
}

void OBSBasicSettings::SaveEncoder(QComboBox *combo, const char *section,
		const char *value)
{
	QVariant v = combo->currentData();
	CodecDesc cd;
	if (!v.isNull())
		cd = v.value<CodecDesc>();
	config_set_int(main->Config(), section,
			QT_TO_UTF8(QString("%1Id").arg(value)), cd.id);
	if (cd.id != 0)
		config_set_string(main->Config(), section, value, cd.name);
	else
		config_set_string(main->Config(), section, value, nullptr);
}

void OBSBasicSettings::SaveOutputSettings()
{
	SaveEdit(ui->advOutFFURL, "AdvOut", "FFURL");
	SaveSpinBox(ui->advOutFFVBitrate, "AdvOut", "FFVBitrate");
	SaveCheckBox(ui->advOutFFUseRescale, "AdvOut", "FFRescale");
	SaveCombo(ui->advOutFFRescale, "AdvOut", "FFRescaleRes");
	SaveSpinBox(ui->advOutFFABitrate, "AdvOut", "FFABitrate");
	SaveTrackIndex(main->Config(), "AdvOut", "FFAudioTrack",
			ui->advOutFFTrack1, ui->advOutFFTrack2,
			ui->advOutFFTrack3, ui->advOutFFTrack4);

	SaveCombo(ui->advOutTrack1Bitrate, "AdvOut", "Track1Bitrate");
	SaveCombo(ui->advOutTrack2Bitrate, "AdvOut", "Track2Bitrate");
	SaveCombo(ui->advOutTrack3Bitrate, "AdvOut", "Track3Bitrate");
	SaveCombo(ui->advOutTrack4Bitrate, "AdvOut", "Track4Bitrate");
	SaveEdit(ui->advOutTrack1Name, "AdvOut", "Track1Name");
	SaveEdit(ui->advOutTrack2Name, "AdvOut", "Track2Name");
	SaveEdit(ui->advOutTrack3Name, "AdvOut", "Track3Name");
	SaveEdit(ui->advOutTrack4Name, "AdvOut", "Track4Name");

	/* Save FTL data */
	SaveEdit(ui->advOutFTLStreamKey, "AdvOut", "FTLStreamKey");

	WriteJsonData(streamEncoderProps, "streamEncoder.json");
	WriteJsonData(recordEncoderProps, "recordEncoder.json");
	main->ResetOutputs();
}

void OBSBasicSettings::SaveAudioSettings()
{
	QString sampleRateStr  = ui->sampleRate->currentText();
	int channelSetupIdx    = ui->channelSetup->currentIndex();

	const char *channelSetup = (channelSetupIdx == 0) ? "Mono" : "Stereo";

	int sampleRate = 44800;
	if (sampleRateStr == "48khz")
		sampleRate = 48000;

	if (WidgetChanged(ui->sampleRate))
		config_set_uint(main->Config(), "Audio", "SampleRate",
				sampleRate);

	if (WidgetChanged(ui->channelSetup))
		config_set_string(main->Config(), "Audio", "ChannelSetup",
				channelSetup);

	for (auto &audioSource : audioSources) {
		auto source  = OBSGetStrongRef(get<0>(audioSource));
		if (!source)
			continue;

		auto &ptmCB  = get<1>(audioSource);
		auto &ptmSB  = get<2>(audioSource);
		auto &pttCB  = get<3>(audioSource);
		auto &pttSB  = get<4>(audioSource);

		obs_source_enable_push_to_mute(source, ptmCB->isChecked());
		obs_source_set_push_to_mute_delay(source, ptmSB->value());

		obs_source_enable_push_to_talk(source, pttCB->isChecked());
		obs_source_set_push_to_talk_delay(source, pttSB->value());
	}

	auto UpdateAudioDevice = [this](bool input, QComboBox *combo,
			const char *name, int index)
	{
		main->ResetAudioDevice(
				input ? App()->InputAudioSource()
				      : App()->OutputAudioSource(),
				QT_TO_UTF8(GetComboData(combo)),
				Str(name), index);
	};

	UpdateAudioDevice(false, ui->desktopAudioDevice1,
			"Basic.DesktopDevice1", 1);
	UpdateAudioDevice(false, ui->desktopAudioDevice2,
			"Basic.DesktopDevice2", 2);
	UpdateAudioDevice(true, ui->auxAudioDevice1,
			"Basic.AuxDevice1", 3);
	UpdateAudioDevice(true, ui->auxAudioDevice2,
			"Basic.AuxDevice2", 4);
	UpdateAudioDevice(true, ui->auxAudioDevice3,
			"Basic.AuxDevice3", 5);
	main->SaveProject();
}

void OBSBasicSettings::SaveHotkeySettings()
{
	const auto &config = main->Config();

	using namespace std;

	std::vector<obs_key_combination> combinations;
	for (auto &hotkey : hotkeys) {
		auto &hw = *hotkey.second;
		if (!hw.Changed())
			continue;

		hw.Save(combinations);

		if (!hotkey.first)
			continue;

		obs_data_array_t *array = obs_hotkey_save(hw.id);
		obs_data_t *data = obs_data_create();
		obs_data_set_array(data, "bindings", array);
		const char *json = obs_data_get_json(data);
		config_set_string(config, "Hotkeys", hw.name.c_str(), json);
		obs_data_release(data);
		obs_data_array_release(array);
	}
}

#define MINOR_SEPARATOR \
	"------------------------------------------------"

static void AddChangedVal(std::string &changed, const char *str)
{
	if (changed.size())
		changed += ", ";
	changed += str;
}

void OBSBasicSettings::SaveSettings()
{
	if (generalChanged)
		SaveGeneralSettings();
	if (stream1Changed)
		SaveStream1Settings();
	if (outputsChanged)
		SaveOutputSettings();
	if (audioChanged)
		SaveAudioSettings();
	if (videoChanged)
		SaveVideoSettings();
	if (hotkeysChanged)
		SaveHotkeySettings();
	if (advancedChanged)
		SaveAdvancedSettings();

	if (videoChanged || advancedChanged)
		main->ResetVideo();

	config_save_safe(main->Config(), "tmp", nullptr);
	config_save_safe(GetGlobalConfig(), "tmp", nullptr);
	main->SaveProject();

	if (Changed()) {
		std::string changed;
		if (generalChanged)
			AddChangedVal(changed, "general");
		if (stream1Changed)
			AddChangedVal(changed, "stream 1");
		if (outputsChanged)
			AddChangedVal(changed, "outputs");
		if (audioChanged)
			AddChangedVal(changed, "audio");
		if (videoChanged)
			AddChangedVal(changed, "video");
		if (hotkeysChanged)
			AddChangedVal(changed, "hotkeys");
		if (advancedChanged)
			AddChangedVal(changed, "advanced");

		blog(LOG_INFO, "Settings changed (%s)", changed.c_str());
		blog(LOG_INFO, MINOR_SEPARATOR);
	}
}

bool OBSBasicSettings::QueryChanges()
{
	QMessageBox::StandardButton button;

	button = QMessageBox::question(this,
			QTStr("Basic.Settings.ConfirmTitle"),
			QTStr("Basic.Settings.Confirm"),
			QMessageBox::Yes | QMessageBox::No |
			QMessageBox::Cancel);

	if (button == QMessageBox::Cancel) {
		return false;
	} else if (button == QMessageBox::Yes) {
		SaveSettings();
	} else {
		LoadSettings(true);
#ifdef _WIN32
		if (toggleAero)
			SetAeroEnabled(!aeroWasDisabled);
#endif
	}

	ClearChanged();
	return true;
}

void OBSBasicSettings::closeEvent(QCloseEvent *event)
{
	if (Changed() && !QueryChanges())
		event->ignore();
}

void OBSBasicSettings::on_theme_activated(int idx)
{
	string currT = ui->theme->itemText(idx).toStdString();
	App()->SetTheme(currT);
}

void OBSBasicSettings::on_listWidget_itemSelectionChanged()
{
	int row = ui->listWidget->currentRow();

	if (loading || row == pageIndex)
		return;

	pageIndex = row;
}

void OBSBasicSettings::on_buttonBox_clicked(QAbstractButton *button)
{
	QDialogButtonBox::ButtonRole val = ui->buttonBox->buttonRole(button);

	if (val == QDialogButtonBox::ApplyRole ||
	    val == QDialogButtonBox::AcceptRole) {
		SaveSettings();
		ClearChanged();
	}

	if (val == QDialogButtonBox::AcceptRole ||
	    val == QDialogButtonBox::RejectRole) {
		if (val == QDialogButtonBox::RejectRole) {
			App()->SetTheme(savedTheme);
#ifdef _WIN32
			if (toggleAero)
				SetAeroEnabled(!aeroWasDisabled);
#endif
		}
		ClearChanged();
		close();
	}
}

void OBSBasicSettings::on_colorFormat_currentIndexChanged(const QString &text)
{
	bool usingNV12 = text == "NV12";

	if (usingNV12)
		ui->advancedMsg2->setText(QString());
	else
		ui->advancedMsg2->setText(
				QTStr("Basic.Settings.Advanced.FormatWarning"));
}

#define INVALID_RES_STR "Basic.Settings.Video.InvalidResolution"

static bool ValidResolutions(Ui::OBSBasicSettings *ui)
{
	QString baseRes   = ui->baseResolution->lineEdit()->text();
	QString outputRes = ui->outputResolution->lineEdit()->text();
	uint32_t cx, cy;

	if (!ConvertResText(QT_TO_UTF8(baseRes), cx, cy) ||
	    !ConvertResText(QT_TO_UTF8(outputRes), cx, cy)) {

		ui->videoMsg->setText(QTStr(INVALID_RES_STR));
		return false;
	}

	ui->videoMsg->setText("");
	return true;
}

void OBSBasicSettings::RecalcOutputResPixels(const char *resText)
{
	uint32_t newCX;
	uint32_t newCY;

	ConvertResText(resText, newCX, newCY);
	if (newCX && newCY) {
		outputCX = newCX;
		outputCY = newCY;
	}
}

void OBSBasicSettings::on_outputResolution_editTextChanged(const QString &text)
{
	if (!loading)
		RecalcOutputResPixels(QT_TO_UTF8(text));
}

void OBSBasicSettings::on_baseResolution_editTextChanged(const QString &text)
{
	if (!loading && ValidResolutions(ui.get())) {
		QString baseResolution = text;
		uint32_t cx, cy;

		ConvertResText(QT_TO_UTF8(baseResolution), cx, cy);
		ResetDownscales(cx, cy);
	}
}

void OBSBasicSettings::GeneralChanged()
{
	if (!loading) {
		generalChanged = true;
		sender()->setProperty("changed", QVariant(true));
		EnableApplyButton(true);
	}
}

void OBSBasicSettings::Stream1Changed()
{
	if (!loading) {
		stream1Changed = true;
		sender()->setProperty("changed", QVariant(true));
		EnableApplyButton(true);
	}
}

void OBSBasicSettings::OutputsChanged()
{
	if (!loading) {
		outputsChanged = true;
		sender()->setProperty("changed", QVariant(true));
		EnableApplyButton(true);
	}
}

void OBSBasicSettings::AudioChanged()
{
	if (!loading) {
		audioChanged = true;
		sender()->setProperty("changed", QVariant(true));
		EnableApplyButton(true);
	}
}

void OBSBasicSettings::AudioChangedRestart()
{
	if (!loading) {
		audioChanged = true;
		ui->audioMsg->setText(QTStr("Basic.Settings.ProgramRestart"));
		sender()->setProperty("changed", QVariant(true));
		EnableApplyButton(true);
	}
}

void OBSBasicSettings::ReloadAudioSources()
{
	LoadAudioSources();
}

void OBSBasicSettings::VideoChangedRestart()
{
	if (!loading) {
		videoChanged = true;
		ui->videoMsg->setText(QTStr("Basic.Settings.ProgramRestart"));
		sender()->setProperty("changed", QVariant(true));
		EnableApplyButton(true);
	}
}

void OBSBasicSettings::AdvancedChangedRestart()
{
	if (!loading) {
		advancedChanged = true;
		ui->advancedMsg->setText(
				QTStr("Basic.Settings.ProgramRestart"));
		sender()->setProperty("changed", QVariant(true));
		EnableApplyButton(true);
	}
}

void OBSBasicSettings::VideoChangedResolution()
{
	if (!loading && ValidResolutions(ui.get())) {
		videoChanged = true;
		sender()->setProperty("changed", QVariant(true));
		EnableApplyButton(true);
	}
}

void OBSBasicSettings::VideoChanged()
{
	if (!loading) {
		videoChanged = true;
		sender()->setProperty("changed", QVariant(true));
		EnableApplyButton(true);
	}
}

void OBSBasicSettings::HotkeysChanged()
{
	using namespace std;
	if (loading)
		return;

	hotkeysChanged = any_of(begin(hotkeys), end(hotkeys),
			[](const pair<bool, QPointer<OBSHotkeyWidget>> &hotkey)
	{
		const auto &hw = *hotkey.second;
		return hw.Changed();
	});

	if (hotkeysChanged)
		EnableApplyButton(true);
}

void OBSBasicSettings::ReloadHotkeys(obs_hotkey_id ignoreKey)
{
	LoadHotkeySettings(ignoreKey);
}

void OBSBasicSettings::AdvancedChanged()
{
	if (!loading) {
		advancedChanged = true;
		sender()->setProperty("changed", QVariant(true));
		EnableApplyButton(true);
	}
}

void OBSBasicSettings::AdvOutRecCheckWarnings()
{
}

static inline QString MakeMemorySizeString(int bitrate, int seconds)
{
	QString str = QTStr("Basic.Settings.Advanced.StreamDelay.MemoryUsage");
	int megabytes = bitrate * seconds / 1000 / 8;

	return str.arg(QString::number(megabytes));
}

void OBSBasicSettings::UpdateSimpleOutStreamDelayEstimate()
{
}

void OBSBasicSettings::UpdateAdvOutStreamDelayEstimate()
{
	if (!streamEncoderProps)
		return;

	OBSData settings = streamEncoderProps->GetSettings();
	int trackIndex = config_get_int(main->Config(), "AdvOut", "TrackIndex");
	QString aBitrateText;

	switch (trackIndex) {
	case 1: aBitrateText = ui->advOutTrack1Bitrate->currentText(); break;
	case 2: aBitrateText = ui->advOutTrack2Bitrate->currentText(); break;
	case 3: aBitrateText = ui->advOutTrack3Bitrate->currentText(); break;
	case 4: aBitrateText = ui->advOutTrack4Bitrate->currentText(); break;
	}

	int seconds = ui->streamDelaySec->value();
	int vBitrate = (int)obs_data_get_int(settings, "bitrate");
	int aBitrate = aBitrateText.toInt();

	QString msg = MakeMemorySizeString(vBitrate + aBitrate, seconds);

	ui->streamDelayInfo->setText(msg);
}

void OBSBasicSettings::UpdateStreamDelayEstimate()
{
}

void OBSBasicSettings::FillSimpleRecordingValues()
{
}

void OBSBasicSettings::SimpleRecordingQualityChanged()
{
}

void OBSBasicSettings::SimpleRecordingEncoderChanged()
{
}

void OBSBasicSettings::SimpleRecordingQualityLosslessWarning(int idx)
{
	UNUSED_PARAMETER(idx);
}

void OBSBasicSettings::on_disableOSXVSync_clicked()
{
#ifdef __APPLE__
	if (!loading) {
		bool disable = ui->disableOSXVSync->isChecked();
		ui->resetOSXVSync->setEnabled(disable);
	}
#endif
}

void OBSBasicSettings::on_advOutFTLIngestLoc_currentIndexChanged(int idx)
{
	/* First we need to get the current index and its value */
	QString ingest_url;
	ingest_url = ui->advOutFTLIngestLoc->itemData(idx).toString();

	/* If the string is empty, allow for a custom URL, else don't */
	if (ingest_url == QString("")) {
		ui->advOutSavePathURLlabel->show();
		ui->advOutFFURL->show();
	} else {
		ui->advOutSavePathURLlabel->hide();
		ui->advOutFFURL->hide();
		ui->advOutFFURL->setText(ingest_url);
	}
}
