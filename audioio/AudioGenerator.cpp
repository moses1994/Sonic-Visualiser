/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
    Sonic Visualiser
    An audio file viewer and annotation editor.
    Centre for Digital Music, Queen Mary, University of London.
    This file copyright 2006 Chris Cannam.
    
    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.
*/

#include "AudioGenerator.h"

#include "base/TempDirectory.h"
#include "base/PlayParameters.h"
#include "base/PlayParameterRepository.h"
#include "base/Pitch.h"
#include "base/Exceptions.h"

#include "data/model/NoteModel.h"
#include "data/model/DenseTimeValueModel.h"
#include "data/model/SparseOneDimensionalModel.h"

#include "plugin/RealTimePluginFactory.h"
#include "plugin/RealTimePluginInstance.h"
#include "plugin/PluginIdentifier.h"
#include "plugin/PluginXml.h"
#include "plugin/api/alsa/seq_event.h"

#include <iostream>
#include <cmath>

#include <QDir>
#include <QFile>

const size_t
AudioGenerator::m_pluginBlockSize = 2048;

QString
AudioGenerator::m_sampleDir = "";

//#define DEBUG_AUDIO_GENERATOR 1

AudioGenerator::AudioGenerator() :
    m_sourceSampleRate(0),
    m_targetChannelCount(1),
    m_soloing(false)
{
    initialiseSampleDir();

    connect(PlayParameterRepository::getInstance(),
            SIGNAL(playPluginIdChanged(const Playable *, QString)),
            this,
            SLOT(playPluginIdChanged(const Playable *, QString)));

    connect(PlayParameterRepository::getInstance(),
            SIGNAL(playPluginConfigurationChanged(const Playable *, QString)),
            this,
            SLOT(playPluginConfigurationChanged(const Playable *, QString)));
}

AudioGenerator::~AudioGenerator()
{
#ifdef DEBUG_AUDIO_GENERATOR
    std::cerr << "AudioGenerator::~AudioGenerator" << std::endl;
#endif
}

void
AudioGenerator::initialiseSampleDir()
{
    if (m_sampleDir != "") return;

    try {
        m_sampleDir = TempDirectory::getInstance()->getSubDirectoryPath("samples");
    } catch (DirectoryCreationFailed f) {
        std::cerr << "WARNING: AudioGenerator::initialiseSampleDir:"
                  << " Failed to create temporary sample directory"
                  << std::endl;
        m_sampleDir = "";
        return;
    }

    QDir sampleResourceDir(":/samples", "*.wav");

    for (unsigned int i = 0; i < sampleResourceDir.count(); ++i) {

        QString fileName(sampleResourceDir[i]);
        QFile file(sampleResourceDir.filePath(fileName));
        QString target = QDir(m_sampleDir).filePath(fileName);

        if (!file.copy(target)) {
            std::cerr << "WARNING: AudioGenerator::getSampleDir: "
                      << "Unable to copy " << fileName.toStdString()
                      << " into temporary directory \""
                      << m_sampleDir.toStdString() << "\"" << std::endl;
        } else {
            QFile tf(target);
            tf.setPermissions(tf.permissions() |
                              QFile::WriteOwner |
                              QFile::WriteUser);
        }
    }
}

bool
AudioGenerator::addModel(Model *model)
{
    if (m_sourceSampleRate == 0) {

	m_sourceSampleRate = model->getSampleRate();

    } else {

	DenseTimeValueModel *dtvm =
	    dynamic_cast<DenseTimeValueModel *>(model);

	if (dtvm) {
	    m_sourceSampleRate = model->getSampleRate();
	    return true;
	}
    }

    RealTimePluginInstance *plugin = loadPluginFor(model);
    if (plugin) {
        QMutexLocker locker(&m_mutex);
        m_synthMap[model] = plugin;
        return true;
    }

    return false;
}

void
AudioGenerator::playPluginIdChanged(const Playable *playable, QString)
{
    const Model *model = dynamic_cast<const Model *>(playable);
    if (!model) {
        std::cerr << "WARNING: AudioGenerator::playPluginIdChanged: playable "
                  << playable << " is not a supported model type"
                  << std::endl;
        return;
    }

    if (m_synthMap.find(model) == m_synthMap.end()) return;
    
    RealTimePluginInstance *plugin = loadPluginFor(model);
    if (plugin) {
        QMutexLocker locker(&m_mutex);
        delete m_synthMap[model];
        m_synthMap[model] = plugin;
    }
}

void
AudioGenerator::playPluginConfigurationChanged(const Playable *playable,
                                               QString configurationXml)
{
//    std::cerr << "AudioGenerator::playPluginConfigurationChanged" << std::endl;

    const Model *model = dynamic_cast<const Model *>(playable);
    if (!model) {
        std::cerr << "WARNING: AudioGenerator::playPluginIdChanged: playable "
                  << playable << " is not a supported model type"
                  << std::endl;
        return;
    }

    if (m_synthMap.find(model) == m_synthMap.end()) {
        std::cerr << "AudioGenerator::playPluginConfigurationChanged: We don't know about this plugin" << std::endl;
        return;
    }

    RealTimePluginInstance *plugin = m_synthMap[model];
    if (plugin) {
        PluginXml(plugin).setParametersFromXml(configurationXml);
    }
}

void
AudioGenerator::setSampleDir(RealTimePluginInstance *plugin)
{
    if (m_sampleDir != "") {
        plugin->configure("sampledir", m_sampleDir.toStdString());
    }
} 

RealTimePluginInstance *
AudioGenerator::loadPluginFor(const Model *model)
{
    QString pluginId, configurationXml;

    const Playable *playable = model;
    if (!playable || !playable->canPlay()) return 0;

    PlayParameters *parameters =
	PlayParameterRepository::getInstance()->getPlayParameters(playable);
    if (parameters) {
        pluginId = parameters->getPlayPluginId();
        configurationXml = parameters->getPlayPluginConfiguration();
    }

    if (pluginId == "") return 0;

    RealTimePluginInstance *plugin = loadPlugin(pluginId, "");
    if (!plugin) return 0;

    if (configurationXml != "") {
        PluginXml(plugin).setParametersFromXml(configurationXml);
        setSampleDir(plugin);
    }

    configurationXml = PluginXml(plugin).toXmlString();

    if (parameters) {
        parameters->setPlayPluginId(pluginId);
        parameters->setPlayPluginConfiguration(configurationXml);
    }

    return plugin;
}

RealTimePluginInstance *
AudioGenerator::loadPlugin(QString pluginId, QString program)
{
    RealTimePluginFactory *factory =
	RealTimePluginFactory::instanceFor(pluginId);
    
    if (!factory) {
	std::cerr << "Failed to get plugin factory" << std::endl;
	return false;
    }
	
    RealTimePluginInstance *instance =
	factory->instantiatePlugin
	(pluginId, 0, 0, m_sourceSampleRate, m_pluginBlockSize, m_targetChannelCount);

    if (!instance) {
	std::cerr << "Failed to instantiate plugin " << pluginId.toStdString() << std::endl;
        return 0;
    }

    setSampleDir(instance);

    for (unsigned int i = 0; i < instance->getParameterCount(); ++i) {
        instance->setParameterValue(i, instance->getParameterDefault(i));
    }
    std::string defaultProgram = instance->getProgram(0, 0);
    if (defaultProgram != "") {
//        std::cerr << "first selecting default program " << defaultProgram << std::endl;
        instance->selectProgram(defaultProgram);
    }
    if (program != "") {
//        std::cerr << "now selecting desired program " << program.toStdString() << std::endl;
        instance->selectProgram(program.toStdString());
    }
    instance->setIdealChannelCount(m_targetChannelCount); // reset!

    return instance;
}

void
AudioGenerator::removeModel(Model *model)
{
    SparseOneDimensionalModel *sodm =
	dynamic_cast<SparseOneDimensionalModel *>(model);
    if (!sodm) return; // nothing to do

    QMutexLocker locker(&m_mutex);

    if (m_synthMap.find(sodm) == m_synthMap.end()) return;

    RealTimePluginInstance *instance = m_synthMap[sodm];
    m_synthMap.erase(sodm);
    delete instance;
}

void
AudioGenerator::clearModels()
{
    QMutexLocker locker(&m_mutex);
    while (!m_synthMap.empty()) {
	RealTimePluginInstance *instance = m_synthMap.begin()->second;
	m_synthMap.erase(m_synthMap.begin());
	delete instance;
    }
}    

void
AudioGenerator::reset()
{
    QMutexLocker locker(&m_mutex);
    for (PluginMap::iterator i = m_synthMap.begin(); i != m_synthMap.end(); ++i) {
	if (i->second) {
	    i->second->silence();
	    i->second->discardEvents();
	}
    }

    m_noteOffs.clear();
}

void
AudioGenerator::setTargetChannelCount(size_t targetChannelCount)
{
    if (m_targetChannelCount == targetChannelCount) return;

//    std::cerr << "AudioGenerator::setTargetChannelCount(" << targetChannelCount << ")" << std::endl;

    QMutexLocker locker(&m_mutex);
    m_targetChannelCount = targetChannelCount;

    for (PluginMap::iterator i = m_synthMap.begin(); i != m_synthMap.end(); ++i) {
	if (i->second) i->second->setIdealChannelCount(targetChannelCount);
    }
}

size_t
AudioGenerator::getBlockSize() const
{
    return m_pluginBlockSize;
}

void
AudioGenerator::setSoloModelSet(std::set<Model *> s)
{
    QMutexLocker locker(&m_mutex);

    m_soloModelSet = s;
    m_soloing = true;
}

void
AudioGenerator::clearSoloModelSet()
{
    QMutexLocker locker(&m_mutex);

    m_soloModelSet.clear();
    m_soloing = false;
}

size_t
AudioGenerator::mixModel(Model *model, size_t startFrame, size_t frameCount,
			 float **buffer, size_t fadeIn, size_t fadeOut)
{
    if (m_sourceSampleRate == 0) {
	std::cerr << "WARNING: AudioGenerator::mixModel: No base source sample rate available" << std::endl;
	return frameCount;
    }

    QMutexLocker locker(&m_mutex);

    Playable *playable = model;
    if (!playable || !playable->canPlay()) return frameCount;

    PlayParameters *parameters =
	PlayParameterRepository::getInstance()->getPlayParameters(playable);
    if (!parameters) return frameCount;

    bool playing = !parameters->isPlayMuted();
    if (!playing) {
#ifdef DEBUG_AUDIO_GENERATOR
        std::cout << "AudioGenerator::mixModel(" << model << "): muted" << std::endl;
#endif
        return frameCount;
    }

    if (m_soloing) {
        if (m_soloModelSet.find(model) == m_soloModelSet.end()) {
#ifdef DEBUG_AUDIO_GENERATOR
            std::cout << "AudioGenerator::mixModel(" << model << "): not one of the solo'd models" << std::endl;
#endif
            return frameCount;
        }
    }

    float gain = parameters->getPlayGain();
    float pan = parameters->getPlayPan();

    DenseTimeValueModel *dtvm = dynamic_cast<DenseTimeValueModel *>(model);
    if (dtvm) {
	return mixDenseTimeValueModel(dtvm, startFrame, frameCount,
				      buffer, gain, pan, fadeIn, fadeOut);
    }

    SparseOneDimensionalModel *sodm = dynamic_cast<SparseOneDimensionalModel *>
	(model);
    if (sodm) {
	return mixSparseOneDimensionalModel(sodm, startFrame, frameCount,
					    buffer, gain, pan, fadeIn, fadeOut);
    }

    NoteModel *nm = dynamic_cast<NoteModel *>(model);
    if (nm) {
	return mixNoteModel(nm, startFrame, frameCount,
			    buffer, gain, pan, fadeIn, fadeOut);
    }

    return frameCount;
}

size_t
AudioGenerator::mixDenseTimeValueModel(DenseTimeValueModel *dtvm,
				       size_t startFrame, size_t frames,
				       float **buffer, float gain, float pan,
				       size_t fadeIn, size_t fadeOut)
{
    static float **channelBuffer = 0;
    static size_t  channelBufSiz = 0;
    static size_t  channelBufCount = 0;

    size_t totalFrames = frames + fadeIn/2 + fadeOut/2;

    size_t modelChannels = dtvm->getChannelCount();

    if (channelBufSiz < totalFrames || channelBufCount < modelChannels) {

        for (size_t c = 0; c < channelBufCount; ++c) {
            delete[] channelBuffer[c];
        }

	delete[] channelBuffer;
        channelBuffer = new float *[modelChannels];

        for (size_t c = 0; c < modelChannels; ++c) {
            channelBuffer[c] = new float[totalFrames];
        }

        channelBufCount = modelChannels;
	channelBufSiz = totalFrames;
    }

    size_t got = 0;

    if (startFrame >= fadeIn/2) {
        got = dtvm->getData(0, modelChannels - 1,
                            startFrame - fadeIn/2,
                            frames + fadeOut/2 + fadeIn/2,
                            channelBuffer);
    } else {
        size_t missing = fadeIn/2 - startFrame;

        for (size_t c = 0; c < modelChannels; ++c) {
            channelBuffer[c] += missing;
        }

        got = dtvm->getData(0, modelChannels - 1,
                            startFrame,
                            frames + fadeOut/2,
                            channelBuffer);

        for (size_t c = 0; c < modelChannels; ++c) {
            channelBuffer[c] -= missing;
        }

        got += missing;
    }	    

    for (size_t c = 0; c < m_targetChannelCount; ++c) {

	size_t sourceChannel = (c % modelChannels);

//	std::cerr << "mixing channel " << c << " from source channel " << sourceChannel << std::endl;

	float channelGain = gain;
	if (pan != 0.0) {
	    if (c == 0) {
		if (pan > 0.0) channelGain *= 1.0 - pan;
	    } else {
		if (pan < 0.0) channelGain *= pan + 1.0;
	    }
	}

	for (size_t i = 0; i < fadeIn/2; ++i) {
	    float *back = buffer[c];
	    back -= fadeIn/2;
	    back[i] += (channelGain * channelBuffer[sourceChannel][i] * i) / fadeIn;
	}

	for (size_t i = 0; i < frames + fadeOut/2; ++i) {
	    float mult = channelGain;
	    if (i < fadeIn/2) {
		mult = (mult * i) / fadeIn;
	    }
	    if (i > frames - fadeOut/2) {
		mult = (mult * ((frames + fadeOut/2) - i)) / fadeOut;
	    }
            float val = channelBuffer[sourceChannel][i];
            if (i >= got) val = 0.f;
	    buffer[c][i] += mult * val;
	}
    }

    return got;
}
  
size_t
AudioGenerator::mixSparseOneDimensionalModel(SparseOneDimensionalModel *sodm,
					     size_t startFrame, size_t frames,
					     float **buffer, float gain, float pan,
					     size_t /* fadeIn */,
					     size_t /* fadeOut */)
{
    RealTimePluginInstance *plugin = m_synthMap[sodm];
    if (!plugin) return 0;

    size_t latency = plugin->getLatency();
    size_t blocks = frames / m_pluginBlockSize;
    
    //!!! hang on -- the fact that the audio callback play source's
    //buffer is a multiple of the plugin's buffer size doesn't mean
    //that we always get called for a multiple of it here (because it
    //also depends on the JACK block size).  how should we ensure that
    //all models write the same amount in to the mix, and that we
    //always have a multiple of the plugin buffer size?  I guess this
    //class has to be queryable for the plugin buffer size & the
    //callback play source has to use that as a multiple for all the
    //calls to mixModel

    size_t got = blocks * m_pluginBlockSize;

#ifdef DEBUG_AUDIO_GENERATOR
    std::cout << "mixModel [sparse]: frames " << frames
	      << ", blocks " << blocks << std::endl;
#endif

    snd_seq_event_t onEv;
    onEv.type = SND_SEQ_EVENT_NOTEON;
    onEv.data.note.channel = 0;
    onEv.data.note.note = 64;
    onEv.data.note.velocity = 100;

    snd_seq_event_t offEv;
    offEv.type = SND_SEQ_EVENT_NOTEOFF;
    offEv.data.note.channel = 0;
    offEv.data.note.velocity = 0;
    
    NoteOffSet &noteOffs = m_noteOffs[sodm];

    for (size_t i = 0; i < blocks; ++i) {

	size_t reqStart = startFrame + i * m_pluginBlockSize;

	SparseOneDimensionalModel::PointList points =
	    sodm->getPoints(reqStart + latency,
			    reqStart + latency + m_pluginBlockSize);

        Vamp::RealTime blockTime = Vamp::RealTime::frame2RealTime
	    (startFrame + i * m_pluginBlockSize, m_sourceSampleRate);

	for (SparseOneDimensionalModel::PointList::iterator pli =
		 points.begin(); pli != points.end(); ++pli) {

	    size_t pliFrame = pli->frame;

	    if (pliFrame >= latency) pliFrame -= latency;

	    if (pliFrame < reqStart ||
		pliFrame >= reqStart + m_pluginBlockSize) continue;

	    while (noteOffs.begin() != noteOffs.end() &&
		   noteOffs.begin()->frame <= pliFrame) {

                Vamp::RealTime eventTime = Vamp::RealTime::frame2RealTime
		    (noteOffs.begin()->frame, m_sourceSampleRate);

		offEv.data.note.note = noteOffs.begin()->pitch;

#ifdef DEBUG_AUDIO_GENERATOR
		std::cerr << "mixModel [sparse]: sending note-off event at time " << eventTime << " frame " << noteOffs.begin()->frame << " pitch " << noteOffs.begin()->pitch << std::endl;
#endif

		plugin->sendEvent(eventTime, &offEv);
		noteOffs.erase(noteOffs.begin());
	    }

            Vamp::RealTime eventTime = Vamp::RealTime::frame2RealTime
		(pliFrame, m_sourceSampleRate);
	    
	    plugin->sendEvent(eventTime, &onEv);

#ifdef DEBUG_AUDIO_GENERATOR
	    std::cout << "mixModel [sparse]: point at frame " << pliFrame << ", block start " << (startFrame + i * m_pluginBlockSize) << ", resulting time " << eventTime << std::endl;
#endif
	    
	    size_t duration = 7000; // frames [for now]
	    NoteOff noff;
	    noff.pitch = onEv.data.note.note;
	    noff.frame = pliFrame + duration;
	    noteOffs.insert(noff);
	}

	while (noteOffs.begin() != noteOffs.end() &&
	       noteOffs.begin()->frame <=
	       startFrame + i * m_pluginBlockSize + m_pluginBlockSize) {

            Vamp::RealTime eventTime = Vamp::RealTime::frame2RealTime
		(noteOffs.begin()->frame, m_sourceSampleRate);

	    offEv.data.note.note = noteOffs.begin()->pitch;

#ifdef DEBUG_AUDIO_GENERATOR
            std::cerr << "mixModel [sparse]: sending leftover note-off event at time " << eventTime << " frame " << noteOffs.begin()->frame << " pitch " << noteOffs.begin()->pitch << std::endl;
#endif

	    plugin->sendEvent(eventTime, &offEv);
	    noteOffs.erase(noteOffs.begin());
	}
	
	plugin->run(blockTime);
	float **outs = plugin->getAudioOutputBuffers();

	for (size_t c = 0; c < m_targetChannelCount; ++c) {
#ifdef DEBUG_AUDIO_GENERATOR
	    std::cout << "mixModel [sparse]: adding " << m_pluginBlockSize << " samples from plugin output " << c << std::endl;
#endif

	    size_t sourceChannel = (c % plugin->getAudioOutputCount());

	    float channelGain = gain;
	    if (pan != 0.0) {
		if (c == 0) {
		    if (pan > 0.0) channelGain *= 1.0 - pan;
		} else {
		    if (pan < 0.0) channelGain *= pan + 1.0;
		}
	    }

	    for (size_t j = 0; j < m_pluginBlockSize; ++j) {
		buffer[c][i * m_pluginBlockSize + j] +=
		    channelGain * outs[sourceChannel][j];
	    }
	}
    }

    return got;
}

    
//!!! mucho duplication with above -- refactor
size_t
AudioGenerator::mixNoteModel(NoteModel *nm,
			     size_t startFrame, size_t frames,
			     float **buffer, float gain, float pan,
			     size_t /* fadeIn */,
			     size_t /* fadeOut */)
{
    RealTimePluginInstance *plugin = m_synthMap[nm];
    if (!plugin) return 0;

    size_t latency = plugin->getLatency();
    size_t blocks = frames / m_pluginBlockSize;
    
    //!!! hang on -- the fact that the audio callback play source's
    //buffer is a multiple of the plugin's buffer size doesn't mean
    //that we always get called for a multiple of it here (because it
    //also depends on the JACK block size).  how should we ensure that
    //all models write the same amount in to the mix, and that we
    //always have a multiple of the plugin buffer size?  I guess this
    //class has to be queryable for the plugin buffer size & the
    //callback play source has to use that as a multiple for all the
    //calls to mixModel

    size_t got = blocks * m_pluginBlockSize;

#ifdef DEBUG_AUDIO_GENERATOR
    Vamp::RealTime startTime = Vamp::RealTime::frame2RealTime
        (startFrame, m_sourceSampleRate);

    std::cout << "mixModel [note]: frames " << frames << " from " << startFrame
	      << " (time " << startTime << "), blocks " << blocks << std::endl;
#endif

    snd_seq_event_t onEv;
    onEv.type = SND_SEQ_EVENT_NOTEON;
    onEv.data.note.channel = 0;
    onEv.data.note.note = 64;
    onEv.data.note.velocity = 100;

    snd_seq_event_t offEv;
    offEv.type = SND_SEQ_EVENT_NOTEOFF;
    offEv.data.note.channel = 0;
    offEv.data.note.velocity = 0;
    
    NoteOffSet &noteOffs = m_noteOffs[nm];

    for (size_t i = 0; i < blocks; ++i) {

	size_t reqStart = startFrame + i * m_pluginBlockSize;

	NoteModel::PointList points =
	    nm->getPoints(reqStart + latency,
			    reqStart + latency + m_pluginBlockSize);

        Vamp::RealTime blockTime = Vamp::RealTime::frame2RealTime
	    (startFrame + i * m_pluginBlockSize, m_sourceSampleRate);

	for (NoteModel::PointList::iterator pli =
		 points.begin(); pli != points.end(); ++pli) {

	    size_t pliFrame = pli->frame;

	    if (pliFrame >= latency) pliFrame -= latency;

	    if (pliFrame < reqStart ||
		pliFrame >= reqStart + m_pluginBlockSize) continue;

	    while (noteOffs.begin() != noteOffs.end() &&
		   noteOffs.begin()->frame <= pliFrame) {

                Vamp::RealTime eventTime = Vamp::RealTime::frame2RealTime
		    (noteOffs.begin()->frame, m_sourceSampleRate);

		offEv.data.note.note = noteOffs.begin()->pitch;

#ifdef DEBUG_AUDIO_GENERATOR
		std::cerr << "mixModel [note]: sending note-off event at time " << eventTime << " frame " << noteOffs.begin()->frame << " pitch " << noteOffs.begin()->pitch << std::endl;
#endif

		plugin->sendEvent(eventTime, &offEv);
		noteOffs.erase(noteOffs.begin());
	    }

            Vamp::RealTime eventTime = Vamp::RealTime::frame2RealTime
		(pliFrame, m_sourceSampleRate);
	    
            if (nm->getScaleUnits() == "Hz") {
                onEv.data.note.note = Pitch::getPitchForFrequency(pli->value);
            } else {
                onEv.data.note.note = lrintf(pli->value);
            }

            if (pli->level > 0.f && pli->level <= 1.f) {
                onEv.data.note.velocity = lrintf(pli->level * 127);
            } else {
                onEv.data.note.velocity = 100;
            }

	    plugin->sendEvent(eventTime, &onEv);

#ifdef DEBUG_AUDIO_GENERATOR
	    std::cout << "mixModel [note]: point at frame " << pliFrame << ", pitch " << (int)onEv.data.note.note << ", block start " << (startFrame + i * m_pluginBlockSize) << ", resulting time " << eventTime << std::endl;
#endif
	    
	    size_t duration = pli->duration;
            if (duration == 0 || duration == 1) {
                duration = m_sourceSampleRate / 20;
            }
	    NoteOff noff;
	    noff.pitch = onEv.data.note.note;
	    noff.frame = pliFrame + duration;
	    noteOffs.insert(noff);

#ifdef DEBUG_AUDIO_GENERATOR
            std::cout << "mixModel [note]: recording note off at " << noff.frame << std::endl;
#endif
	}

	while (noteOffs.begin() != noteOffs.end() &&
	       noteOffs.begin()->frame <=
	       startFrame + i * m_pluginBlockSize + m_pluginBlockSize) {

            Vamp::RealTime eventTime = Vamp::RealTime::frame2RealTime
		(noteOffs.begin()->frame, m_sourceSampleRate);

	    offEv.data.note.note = noteOffs.begin()->pitch;

#ifdef DEBUG_AUDIO_GENERATOR
            std::cerr << "mixModel [note]: sending leftover note-off event at time " << eventTime << " frame " << noteOffs.begin()->frame << " pitch " << noteOffs.begin()->pitch << std::endl;
#endif

	    plugin->sendEvent(eventTime, &offEv);
	    noteOffs.erase(noteOffs.begin());
	}
	
	plugin->run(blockTime);
	float **outs = plugin->getAudioOutputBuffers();

	for (size_t c = 0; c < m_targetChannelCount; ++c) {
#ifdef DEBUG_AUDIO_GENERATOR
	    std::cout << "mixModel [note]: adding " << m_pluginBlockSize << " samples from plugin output " << c << std::endl;
#endif

	    size_t sourceChannel = (c % plugin->getAudioOutputCount());

	    float channelGain = gain;
	    if (pan != 0.0) {
		if (c == 0) {
		    if (pan > 0.0) channelGain *= 1.0 - pan;
		} else {
		    if (pan < 0.0) channelGain *= pan + 1.0;
		}
	    }

	    for (size_t j = 0; j < m_pluginBlockSize; ++j) {
		buffer[c][i * m_pluginBlockSize + j] += 
		    channelGain * outs[sourceChannel][j];
	    }
	}
    }

    return got;
}

