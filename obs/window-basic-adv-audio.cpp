#include <QVBoxLayout>
#include <QGridLayout>
#include <QScrollArea>
#include <QLabel>
#include "window-basic-adv-audio.hpp"
#include "window-basic-main.hpp"
#include "adv-audio-control.hpp"
#include "obs-app.hpp"
#include "qt-wrappers.hpp"

Q_DECLARE_METATYPE(OBSSource);

OBSBasicAdvAudio::OBSBasicAdvAudio(QWidget *parent)
	: QDialog(parent),
	  sourceAddedSignal(obs_get_signal_handler(), "source_activate",
			  OBSSourceAdded, this),
	  sourceRemovedSignal(obs_get_signal_handler(), "source_deactivate",
			  OBSSourceRemoved, this)
{
	QScrollArea *scrollArea;
	QVBoxLayout *vlayout;
	QWidget *widget;
	QLabel *label;

	mainLayout = new QGridLayout;
	mainLayout->setContentsMargins(0, 0, 0, 0);
	label = new QLabel(QTStr("Basic.AdvAudio.Name"));
	label->setAlignment(Qt::AlignHCenter);
	mainLayout->addWidget(label, 0, 0);
	label = new QLabel(QTStr("Basic.AdvAudio.Volume"));
	label->setAlignment(Qt::AlignHCenter);
	mainLayout->addWidget(label, 0, 1);
	label = new QLabel(QTStr("Basic.AdvAudio.Mono"));
	label->setAlignment(Qt::AlignHCenter);
	mainLayout->addWidget(label, 0, 2);
	label = new QLabel(QTStr("Basic.AdvAudio.Panning"));
	label->setAlignment(Qt::AlignHCenter);
	mainLayout->addWidget(label, 0, 3);
	label = new QLabel(QTStr("Basic.AdvAudio.SyncOffset"));
	label->setAlignment(Qt::AlignHCenter);
	mainLayout->addWidget(label, 0, 4);
	label = new QLabel(QTStr("Basic.AdvAudio.AudioTracks"));
	label->setAlignment(Qt::AlignHCenter);
	mainLayout->addWidget(label, 0, 5);

	controlArea = new QWidget;
	controlArea->setLayout(mainLayout);
	controlArea->setSizePolicy(QSizePolicy::Preferred,
			QSizePolicy::Preferred);

	vlayout = new QVBoxLayout;
	vlayout->addWidget(controlArea);
	//vlayout->setAlignment(controlArea, Qt::AlignTop);
	widget = new QWidget;
	widget->setLayout(vlayout);
	widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

	scrollArea = new QScrollArea;
	scrollArea->setWidget(widget);
	scrollArea->setWidgetResizable(true);

	vlayout = new QVBoxLayout;
	vlayout->setContentsMargins(11, 11, 11, 11);
	vlayout->addWidget(scrollArea);
	setLayout(vlayout);

	installEventFilter(CreateShortcutFilter());

	/* get global audio sources */
	for (uint32_t i = 1; i <= 10; i++) {
		obs_source_t *source = obs_get_output_source(i);
		if (source) {
			AddAudioSource(source);
			obs_source_release(source);
		}
	}

	/* enum user scene/sources */
	obs_enum_sources(EnumSources, this);

	resize(1000, 340);
	setWindowTitle(QTStr("Basic.AdvAudio"));
	setSizeGripEnabled(true);
	setWindowModality(Qt::NonModal);
	setAttribute(Qt::WA_DeleteOnClose, true);
}

OBSBasicAdvAudio::~OBSBasicAdvAudio()
{
	OBSBasic *main = reinterpret_cast<OBSBasic*>(parent());

	for (size_t i = 0; i < controls.size(); ++i)
		delete controls[i];

	main->SaveProject();
}

bool OBSBasicAdvAudio::EnumSources(void *param, obs_source_t *source)
{
	OBSBasicAdvAudio *dialog = reinterpret_cast<OBSBasicAdvAudio*>(param);
	uint32_t flags = obs_source_get_output_flags(source);

	if ((flags & OBS_SOURCE_AUDIO) != 0 && obs_source_active(source))
		dialog->AddAudioSource(source);

	return true;
}

void OBSBasicAdvAudio::OBSSourceAdded(void *param, calldata_t *calldata)
{
	OBSSource source((obs_source_t*)calldata_ptr(calldata, "source"));

	QMetaObject::invokeMethod(reinterpret_cast<OBSBasicAdvAudio*>(param),
			"SourceAdded", Q_ARG(OBSSource, source));
}

void OBSBasicAdvAudio::OBSSourceRemoved(void *param, calldata_t *calldata)
{
	OBSSource source((obs_source_t*)calldata_ptr(calldata, "source"));

	QMetaObject::invokeMethod(reinterpret_cast<OBSBasicAdvAudio*>(param),
			"SourceRemoved", Q_ARG(OBSSource, source));
}

inline void OBSBasicAdvAudio::AddAudioSource(obs_source_t *source)
{
	OBSAdvAudioCtrl *control = new OBSAdvAudioCtrl(mainLayout, source);
	controls.push_back(control);
}

void OBSBasicAdvAudio::SourceAdded(OBSSource source)
{
	uint32_t flags = obs_source_get_output_flags(source);

	if ((flags & OBS_SOURCE_AUDIO) == 0)
		return;

	AddAudioSource(source);
}

void OBSBasicAdvAudio::SourceRemoved(OBSSource source)
{
	uint32_t flags = obs_source_get_output_flags(source);

	if ((flags & OBS_SOURCE_AUDIO) == 0)
		return;

	for (size_t i = 0; i < controls.size(); i++) {
		if (controls[i]->GetSource() == source) {
			delete controls[i];
			controls.erase(controls.begin() + i);
			break;
		}
	}
}
