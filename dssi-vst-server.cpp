// -*- c-basic-offset: 4 -*-

/*
  dssi-vst: a DSSI plugin wrapper for VST effects and instruments
  Copyright 2004 Chris Cannam
*/

#include <iostream>
#include <fstream>
#include <vector>
#include <string>

#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/un.h>

#include <unistd.h>
#include <sched.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "aeffectx.h"

#include "remotepluginserver.h"

#include "paths.h"

#define APPLICATION_CLASS_NAME "dssi_vst"
#define PLUGIN_ENTRY_POINT "main"

struct Rect {
    short top;
    short left;
    short bottom;
    short right;
};

static bool inProcessThread = false;
static bool exiting = false;
static HWND hWnd = 0;
static double currentSamplePosition = 0.0;

static bool ready = false;
static int bufferSize = 0;
static int sampleRate = 0;

static RemotePluginDebugLevel debugLevel = RemotePluginDebugSetup;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

using namespace std;

class RemoteVSTServer : public RemotePluginServer
{
public:
    RemoteVSTServer(std::string fileIdentifiers, AEffect *plugin, std::string fallbackName);
    virtual ~RemoteVSTServer();
 
    virtual bool         isReady() { return ready; }
   
    virtual std::string  getName() { return m_name; }
    virtual std::string  getMaker() { return m_maker; }
    virtual void         setBufferSize(int);
    virtual void         setSampleRate(int);
    virtual void         reset();
    virtual void         terminate();
    
    virtual int          getInputCount() { return m_plugin->numInputs; }
    virtual int          getOutputCount() { return m_plugin->numOutputs; }

    virtual int          getParameterCount() { return m_plugin->numParams; }
    virtual std::string  getParameterName(int);
    virtual void         setParameter(int, float);
    virtual float        getParameter(int);
    virtual float        getParameterDefault(int);

    virtual int          getProgramCount() { return m_plugin->numPrograms; }
    virtual std::string  getProgramName(int);
    virtual void         setCurrentProgram(int);

    virtual bool         hasMIDIInput() { return m_hasMIDI; }
    virtual void         sendMIDIData(unsigned char *data,
				      int *frameOffsets,
				      int events);

    virtual void process(float **inputs, float **outputs) {
	if (pthread_mutex_trylock(&mutex)) {
	    for (int i = 0; i < m_plugin->numOutputs; ++i) {
		memset(outputs[i], 0, bufferSize * sizeof(float));
	    }
	    currentSamplePosition += bufferSize;
	    return;
	}

	inProcessThread = true;

	// superclass guarantees setBufferSize will be called before this
	m_plugin->processReplacing(m_plugin, inputs, outputs, bufferSize);
	currentSamplePosition += bufferSize;

	inProcessThread = false;
	pthread_mutex_unlock(&mutex);
    }

    virtual void setDebugLevel(RemotePluginDebugLevel level) {
	debugLevel = level;
    }

    virtual bool warn(std::string);

private:
    AEffect *m_plugin;
    std::string m_name;
    std::string m_maker;
    float *m_defaults;
    bool m_hasMIDI;
};

static RemoteVSTServer *remoteVSTServerInstance = 0;

RemoteVSTServer::RemoteVSTServer(std::string fileIdentifiers,
				 AEffect *plugin, std::string fallbackName) :
    RemotePluginServer(fileIdentifiers),
    m_plugin(plugin),
    m_name(fallbackName),
    m_maker("")
{
    pthread_mutex_lock(&mutex);

    if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: opening plugin" << endl;
    }

    m_plugin->dispatcher(m_plugin, effOpen, 0, 0, NULL, 0);
    m_plugin->dispatcher(m_plugin, effMainsChanged, 0, 0, NULL, 0);

    m_hasMIDI = false;

    if (m_plugin->dispatcher(m_plugin, effGetVstVersion, 0, 0, NULL, 0) < 2) {
	if (debugLevel > 0) {
	    cerr << "dssi-vst-server[1]: plugin is VST 1.x" << endl;
	}
    } else {
	if (debugLevel > 0) {
	    cerr << "dssi-vst-server[1]: plugin is VST 2.0 or newer" << endl;
	}
	if ((m_plugin->flags & effFlagsIsSynth)) {
	    if (debugLevel > 0) {
		cerr << "dssi-vst-server[1]: plugin is a synth" << endl;
	    }
	    m_hasMIDI = true;
	} else {
	    if (debugLevel > 0) {
		cerr << "dssi-vst-server[1]: plugin is not a synth" << endl;
	    }
	    if (m_plugin->dispatcher(m_plugin, effCanDo, 0, 0, (void *)"receiveVstMidiEvent", 0) > 0) {
		if (debugLevel > 0) {
		    cerr << "dssi-vst-server[1]: plugin can receive MIDI anyway" << endl;
		}
		m_hasMIDI = true;
	    }
	}
    }

    char buffer[65];
    buffer[0] = '\0';
    m_plugin->dispatcher(m_plugin, effGetEffectName, 0, 0, buffer, 0);
    if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: plugin name is \"" << buffer
	     << "\"" << endl;
    }
    if (buffer[0]) m_name = buffer;

    buffer[0] = '\0';
    m_plugin->dispatcher(m_plugin, effGetVendorString, 0, 0, buffer, 0);
    if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: vendor string is \"" << buffer
	     << "\"" << endl;
    }
    if (buffer[0]) m_maker = buffer;

    m_plugin->dispatcher(m_plugin, effMainsChanged, 0, 1, NULL, 0);

    m_defaults = new float[m_plugin->numParams];
    for (int i = 0; i < m_plugin->numParams; ++i) {
	m_defaults[i] = m_plugin->getParameter(m_plugin, i);
    }

    pthread_mutex_unlock(&mutex);
}

RemoteVSTServer::~RemoteVSTServer()
{
    pthread_mutex_lock(&mutex);

    m_plugin->dispatcher(m_plugin, effClose, 0, 0, NULL, 0);
    delete[] m_defaults;

    pthread_mutex_unlock(&mutex);
}

void
RemoteVSTServer::setBufferSize(int sz)
{
    pthread_mutex_lock(&mutex);

    m_plugin->dispatcher(m_plugin, effMainsChanged, 0, 0, NULL, 0);
    m_plugin->dispatcher(m_plugin, effSetBlockSize, 0, sz, NULL, 0);
    m_plugin->dispatcher(m_plugin, effMainsChanged, 0, 1, NULL, 0);

    bufferSize = sz;

    if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: set buffer size to " << sz << endl;
    }

    pthread_mutex_unlock(&mutex);
}

void
RemoteVSTServer::setSampleRate(int sr)
{
    pthread_mutex_lock(&mutex);

    m_plugin->dispatcher(m_plugin, effMainsChanged, 0, 0, NULL, 0);
    m_plugin->dispatcher(m_plugin, effSetSampleRate, 0, 0, NULL, (float)sr);
    m_plugin->dispatcher(m_plugin, effMainsChanged, 0, 1, NULL, 0);

    sampleRate = sr;

    if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: set sample rate to " << sr << endl;
    }

    pthread_mutex_unlock(&mutex);
}

void
RemoteVSTServer::reset()
{
    pthread_mutex_lock(&mutex);

    m_plugin->dispatcher(m_plugin, effMainsChanged, 0, 0, NULL, 0);
    m_plugin->dispatcher(m_plugin, effMainsChanged, 0, 1, NULL, 0);

    pthread_mutex_unlock(&mutex);
}

void
RemoteVSTServer::terminate()
{
    cerr << "RemoteVSTServer::terminate: setting exiting flag" << endl;
    exiting = true;
}

std::string
RemoteVSTServer::getParameterName(int p)
{
    char name[24];
    m_plugin->dispatcher(m_plugin, effGetParamName, p, 0, name, 0);
    return name;
}

void
RemoteVSTServer::setParameter(int p, float v)
{
    m_plugin->setParameter(m_plugin, p, v);
}

float
RemoteVSTServer::getParameter(int p)
{
    return m_plugin->getParameter(m_plugin, p);
}

float
RemoteVSTServer::getParameterDefault(int p)
{
    return m_defaults[p];
}

std::string
RemoteVSTServer::getProgramName(int p)
{
    pthread_mutex_lock(&mutex);

    char name[24];
    // effGetProgramName appears to return the name of the current
    // program, not program <index> -- though we pass in <index> as
    // well, just in case
    long prevProgram =
	m_plugin->dispatcher(m_plugin, effGetProgram, 0, 0, NULL, 0);
    m_plugin->dispatcher(m_plugin, effSetProgram, 0, p, NULL, 0);
    m_plugin->dispatcher(m_plugin, effGetProgramName, p, 0, name, 0);
    m_plugin->dispatcher(m_plugin, effSetProgram, 0, prevProgram, NULL, 0);

    pthread_mutex_unlock(&mutex);
    return name;
}

void
RemoteVSTServer::setCurrentProgram(int p)
{
    pthread_mutex_lock(&mutex);

    m_plugin->dispatcher(m_plugin, effSetProgram, 0, p, 0, 0);

    pthread_mutex_unlock(&mutex);
}

void
RemoteVSTServer::sendMIDIData(unsigned char *data, int *frameOffsets, int events)
{
#define MIDI_EVENT_BUFFER_COUNT 1024
    static VstMidiEvent vme[MIDI_EVENT_BUFFER_COUNT];
    static char evbuf[sizeof(VstMidiEvent *) * MIDI_EVENT_BUFFER_COUNT +
		      sizeof(VstEvents)];
    
    VstEvents *vstev = (VstEvents *)evbuf;
    vstev->reserved = 0;

    int ix = 0;

    if (events > MIDI_EVENT_BUFFER_COUNT) {
	std::cerr << "vstserv: WARNING: " << events << " MIDI events received "
		  << "for " << MIDI_EVENT_BUFFER_COUNT << "-event buffer"
		  << std::endl;
	events = MIDI_EVENT_BUFFER_COUNT;
    }

    while (ix < events) {

	vme[ix].type = kVstMidiType;
	vme[ix].byteSize = 24;
	vme[ix].deltaFrames = (frameOffsets ? frameOffsets[ix] : 0);
	vme[ix].flags = 0;
	vme[ix].noteLength = 0;
	vme[ix].noteOffset = 0;
	vme[ix].detune = 0;
	vme[ix].noteOffVelocity = 0;
	vme[ix].reserved1 = 0;
	vme[ix].reserved2 = 0;
	vme[ix].midiData[0] = data[ix*3];
	vme[ix].midiData[1] = data[ix*3+1];
	vme[ix].midiData[2] = data[ix*3+2];
	vme[ix].midiData[3] = 0;
	
	vstev->events[ix] = (VstEvent *)&vme[ix];
	
	if (debugLevel > 1) {
	    cerr << "dssi-vst-server[2]: MIDI event in: "
		 << (int)data[ix*3]   << " "
		 << (int)data[ix*3+1] << " "
		 << (int)data[ix*3+2] << endl;
	}
	
	++ix;
    }

    pthread_mutex_lock(&mutex);

    vstev->numEvents = events;
    if (!m_plugin->dispatcher(m_plugin, effProcessEvents, 0, 0, vstev, 0)) {
	cerr << "WARNING: " << ix << " MIDI event(s) rejected by plugin" << endl;
    }

    pthread_mutex_unlock(&mutex);
}

bool
RemoteVSTServer::warn(std::string warning)
{
    if (hWnd) MessageBox(hWnd, warning.c_str(), "Error", 0);
    return true;
}


long VSTCALLBACK
hostCallback(AEffect *plugin, long opcode, long index,
	     long value, void *ptr, float opt)
{
    static VstTimeInfo timeInfo;

    switch (opcode) {

    case audioMasterAutomate:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterAutomate requested" << endl;
	break;

    case audioMasterVersion:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterVersion requested" << endl;
	return 2300;

    case audioMasterCurrentId:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterCurrentId requested" << endl;
	return 0;

    case audioMasterIdle:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterIdle requested" << endl;
	plugin->dispatcher(plugin, effEditIdle, 0, 0, 0, 0);
	break;

    case audioMasterPinConnected:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterPinConnected requested" << endl;
	break;

    case audioMasterWantMidi:
	if (debugLevel > 1) {
	    cerr << "dssi-vst-server[2]: audioMasterWantMidi requested" << endl;
	}
	// happy to oblige
	return 1;

    case audioMasterGetTime:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetTime requested" << endl;
	timeInfo.samplePos = currentSamplePosition;
	timeInfo.sampleRate = sampleRate;
	timeInfo.flags = 0; // don't mark anything valid except default samplePos/Rate
	return (long)&timeInfo;

    case audioMasterProcessEvents:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterProcessEvents requested" << endl;
	break;

    case audioMasterSetTime:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterSetTime requested" << endl;
	break;

    case audioMasterTempoAt:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterTempoAt requested" << endl;
	// can't support this, return 120bpm
	return 120 * 10000;

    case audioMasterGetNumAutomatableParameters:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetNumAutomatableParameters requested" << endl;
	return 5000;

    case audioMasterGetParameterQuantization :
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetParameterQuantization requested" << endl;
	return 1;

    case audioMasterIOChanged:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterIOChanged requested" << endl;
	cerr << "WARNING: Plugin inputs and/or outputs changed: NOT SUPPORTED" << endl;
	break;

    case audioMasterNeedIdle:
	if (debugLevel > 1) {
	    cerr << "dssi-vst-server[2]: audioMasterNeedIdle requested" << endl;
	}
	// might be nice to handle this better
	return 1;

    case audioMasterSizeWindow:
	if (debugLevel > 1) {
	    cerr << "dssi-vst-server[2]: audioMasterSizeWindow requested" << endl;
	}
	if (hWnd) {
	    SetWindowPos(hWnd, 0, 0, 0,
			 index + 6,
			 value + 25,
			 SWP_NOACTIVATE | SWP_NOMOVE |
			 SWP_NOOWNERZORDER | SWP_NOZORDER);
	}
	return 1;

    case audioMasterGetSampleRate:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetSampleRate requested" << endl;
	if (!sampleRate) {
	    cerr << "WARNING: Sample rate requested but not yet set" << endl;
	}
	plugin->dispatcher(plugin, effSetSampleRate,
			   0, 0, NULL, (float)sampleRate);
	break;

    case audioMasterGetBlockSize:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetBlockSize requested" << endl;
	if (!bufferSize) {
	    cerr << "WARNING: Buffer size requested but not yet set" << endl;
	}
	plugin->dispatcher(plugin, effSetBlockSize,
			   0, bufferSize, NULL, 0);
	break;

    case audioMasterGetInputLatency:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetInputLatency requested" << endl;
	break;

    case audioMasterGetOutputLatency:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetOutputLatency requested" << endl;
	break;

    case audioMasterGetPreviousPlug:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetPreviousPlug requested" << endl;
	break;

    case audioMasterGetNextPlug:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetNextPlug requested" << endl;
	break;

    case audioMasterWillReplaceOrAccumulate:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterWillReplaceOrAccumulate requested" << endl;
	// 0 -> unsupported, 1 -> replace, 2 -> accumulate
	return 1;

    case audioMasterGetCurrentProcessLevel:
	if (debugLevel > 1) {
	    cerr << "dssi-vst-server[2]: audioMasterGetCurrentProcessLevel requested (level is " << (inProcessThread ? 2 : 1) << ")" << endl;
	}
	// 0 -> unsupported, 1 -> gui, 2 -> process, 3 -> midi/timer, 4 -> offline
	if (inProcessThread) return 2;
	else return 1;

    case audioMasterGetAutomationState:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetAutomationState requested" << endl;
	return 4; // read/write

    case audioMasterOfflineStart:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterOfflineStart requested" << endl;
	break;

    case audioMasterOfflineRead:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterOfflineRead requested" << endl;
	break;

    case audioMasterOfflineWrite:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterOfflineWrite requested" << endl;
	break;

    case audioMasterOfflineGetCurrentPass:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterOfflineGetCurrentPass requested" << endl;
	break;

    case audioMasterOfflineGetCurrentMetaPass:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterOfflineGetCurrentMetaPass requested" << endl;
	break;

    case audioMasterSetOutputSampleRate:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterSetOutputSampleRate requested" << endl;
	break;

    case audioMasterGetSpeakerArrangement:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetSpeakerArrangement requested" << endl;
	break;

    case audioMasterGetVendorString:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetVendorString requested" << endl;
	strcpy((char *)ptr, "Fervent Software");
	break;

    case audioMasterGetProductString:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetProductString requested" << endl;
	strcpy((char *)ptr, "DSSI VST Wrapper Plugin");
	break;

    case audioMasterGetVendorVersion:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetVendorVersion requested" << endl;
	return long(RemotePluginVersion * 100);

    case audioMasterVendorSpecific:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterVendorSpecific requested" << endl;
	break;

    case audioMasterSetIcon:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterSetIcon requested" << endl;
	break;

    case audioMasterCanDo:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterCanDo(" << (char *)ptr
		 << ") requested" << endl;
	if (!strcmp((char*)ptr, "sendVstEvents") ||
	    !strcmp((char*)ptr, "sendVstMidiEvent") ||
	    !strcmp((char*)ptr, "sendVstTimeInfo") ||
	    !strcmp((char*)ptr, "sizeWindow") /* ||
	    !strcmp((char*)ptr, "supplyIdle") */) {
	    return 1;
	}
	break;

    case audioMasterGetLanguage:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetLanguage requested" << endl;
	return kVstLangEnglish;

    case audioMasterOpenWindow:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterOpenWindow requested" << endl;
	break;

    case audioMasterCloseWindow:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterCloseWindow requested" << endl;
	break;

    case audioMasterGetDirectory:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetDirectory requested" << endl;
	break;

    case audioMasterUpdateDisplay:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterUpdateDisplay requested" << endl;
	break;

    case audioMasterBeginEdit:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterBeginEdit requested" << endl;
	break;

    case audioMasterEndEdit:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterEndEdit requested" << endl;
	break;

    case audioMasterOpenFileSelector:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterOpenFileSelector requested" << endl;
	break;

    case audioMasterCloseFileSelector:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterCloseFileSelector requested" << endl;
	break;

    case audioMasterEditFile:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterEditFile requested" << endl;
	break;

    case audioMasterGetChunkFile:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetChunkFile requested" << endl;
	break;

    case audioMasterGetInputSpeakerArrangement:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetInputSpeakerArrangement requested" << endl;
	break;

    default:
	if (debugLevel > 0) {
	    cerr << "dssi-vst-server[0]: unsupported audioMaster callback opcode "
		 << opcode << endl;
	}
    }

    return 0;
};

DWORD WINAPI
AudioThreadMain(LPVOID parameter)
{
    struct sched_param param;
    param.sched_priority = 1;
    int result = sched_setscheduler(0, SCHED_FIFO, &param);
    if (result < 0) {
	perror("Failed to set realtime priority for audio thread");
    }

    while (1) {
	try {
	    remoteVSTServerInstance->dispatch();
	} catch (std::string message) {
	    cerr << "ERROR: Remote VST server instance failed: " << message << endl;
	    exiting = true;
	} catch (RemotePluginClosedException) {
	    cerr << "ERROR: Remote VST plugin communication failure" << endl;
	    exiting = true;
	}

	if (exiting) {
	    cerr << "Remote VST plugin audio thread: returning" << endl;
	    param.sched_priority = 0;
	    (void)sched_setscheduler(0, SCHED_OTHER, &param);
	    return 0;
	}
    }
}

LRESULT WINAPI
MainProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_DESTROY:
	PostQuitMessage(0);
	exiting = true;
	return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI
WinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPSTR cmdline, int cmdshow)
{
    char *libname = 0;
    char *fileInfo = 0;
    bool tryGui = false, haveGui = true;

    cout << "DSSI VST plugin server v" << RemotePluginVersion << endl;
    cout << "Copyright (c) 2004 Chris Cannam - Fervent Software" << endl;

    char *home = getenv("HOME");

    if (cmdline) {
	int offset = 0;
	if (cmdline[0] == '"' || cmdline[0] == '\'') offset = 1;
	if (!strncmp(&cmdline[offset], "-g ", 3)) {
	    tryGui = true;
	    offset += 3;
	}
	for (int ci = offset; cmdline[ci]; ++ci) {
	    if (cmdline[ci] == ',') {
		libname = strndup(cmdline + offset, ci - offset);
		++ci;
		if (cmdline[ci]) {
		    fileInfo = strdup(cmdline + ci);
		    int l = strlen(fileInfo);
		    if (fileInfo[l-1] == '"' ||
			fileInfo[l-1] == '\'') {
			fileInfo[l-1] = '\0';
		    }
		}
	    }
	}
    }

    if (!libname || !libname[0] || !fileInfo || !fileInfo[0]) {
	cerr << "Usage: dssi-vst-server <vstname.dll>,<tmpfilebase>" << endl;
	cerr << "(Command line was: " << cmdline << ")" << endl;
	exit(2);
    }

    // LADSPA labels can't contain spaces so dssi-vst replaces spaces
    // with asterisks.
    for (int ci = 0; libname[ci]; ++ci) {
	if (libname[ci] == '*') libname[ci] = ' ';
    }

    cout << "Loading \"" << libname << "\"... ";
    if (debugLevel > 0) cout << endl;

    HINSTANCE libHandle = 0;

    std::vector<std::string> vstPath = Paths::getPath
	("VST_PATH", "/usr/local/lib/vst:/usr/lib/vst", "/vst");

    for (size_t i = 0; i < vstPath.size(); ++i) {
	
	std::string vstDir = vstPath[i];
	std::string libPath;

	if (vstDir[vstDir.length()-1] == '/') {
	    libPath = vstDir + libname;
	} else {
	    libPath = vstDir + "/" + libname;
	}

	libHandle = LoadLibrary(libPath.c_str());
	if (debugLevel > 0) {
	    cerr << "dssi-vst-server[1]: " << (libHandle ? "" : "not ")
		 << "found in " << libPath << endl;
	}

	if (!libHandle) {
	    if (home && home[0] != '\0') {
		if (libPath.substr(0, strlen(home)) == home) {
		    libPath = libPath.substr(strlen(home) + 1);
		}
		libHandle = LoadLibrary(libPath.c_str());
		if (debugLevel > 0) {
		    cerr << "dssi-vst-server[1]: " << (libHandle ? "" : "not ")
			 << "found in " << libPath << endl;
		}
	    }
	}

	if (libHandle) break;
    }	

    if (!libHandle) {
	libHandle = LoadLibrary(libname);
	if (debugLevel > 0) {
	    cerr << "dssi-vst-server[1]: " << (libHandle ? "" : "not ")
		 << "found in DLL path" << endl;
	}
    }

    if (!libHandle) {
	cerr << "dssi-vst-server: ERROR: Couldn't load VST DLL \"" << libname << "\"" << endl;
	return 1;
    }

    cout << "done" << endl;

    cout << "Testing VST compatibility... ";
    if (debugLevel > 0) cout << endl;

//!!! better debug level support
    
    AEffect *(__stdcall* getInstance)(audioMasterCallback);
    getInstance = (AEffect*(__stdcall*)(audioMasterCallback))
	GetProcAddress(libHandle, PLUGIN_ENTRY_POINT);

    if (!getInstance) {
	cerr << "dssi-vst-server: ERROR: VST entrypoint \"" << PLUGIN_ENTRY_POINT
	     << "\" not found in DLL \"" << libname << "\"" << endl;
	return 1;
    } else if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: VST entrypoint \"" << PLUGIN_ENTRY_POINT
	     << "\" found" << endl;
    }

    AEffect *plugin = getInstance(hostCallback);

    if (!plugin) {
	cerr << "dssi-vst-server: ERROR: Failed to instantiate plugin in VST DLL \""
	     << libname << "\"" << endl;
	return 1;
    } else if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: plugin instantiated" << endl;
    }

    if (plugin->magic != kEffectMagic) {
	cerr << "dssi-vst-server: ERROR: Not a VST plugin in DLL \"" << libname << "\"" << endl;
	return 1;
    } else if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: plugin is a VST" << endl;
    }

    if (tryGui) {
	if (!(plugin->flags & effFlagsHasEditor)) {
	    if (debugLevel > 0) {
		cerr << "dssi-vst-server[1]: Plugin has no GUI" << endl;
	    }
	    haveGui = false;
	} else if (debugLevel > 0) {
	    cerr << "dssi-vst-server[1]: plugin has a GUI" << endl;
	}
    }

    if (!plugin->flags & effFlagsCanReplacing) {
	cerr << "dssi-vst-server: ERROR: Plugin does not support processReplacing (required)"
	     << endl;
	return 1;
    } else if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: plugin supports processReplacing" << endl;
    }

    try {
	remoteVSTServerInstance =
	    new RemoteVSTServer(fileInfo, plugin, libname);
    } catch (std::string message) {
	cerr << "ERROR: Remote VST startup failed: " << message << endl;
	return 1;
    } catch (RemotePluginClosedException) {
	cerr << "ERROR: Remote VST plugin communication failure" << endl;
	return 1;
    }

    if (tryGui) {

	cout << "Initialising Windows subsystem... ";
	if (debugLevel > 0) cout << endl;

	WNDCLASSEX wclass;
	wclass.cbSize = sizeof(WNDCLASSEX);
	wclass.style = 0;
	wclass.lpfnWndProc = MainProc;
	wclass.cbClsExtra = 0;
	wclass.cbWndExtra = 0;
	wclass.hInstance = hInst;
	wclass.hIcon = LoadIcon(hInst, APPLICATION_CLASS_NAME);
	wclass.hCursor = LoadCursor(0, IDI_APPLICATION);
	wclass.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	wclass.lpszMenuName = "MENU_DSSI_VST";
	wclass.lpszClassName = APPLICATION_CLASS_NAME;
	wclass.hIconSm = 0;
	
	if (!RegisterClassEx(&wclass)) {
	    cerr << "dssi-vst-server: ERROR: Failed to register Windows application class!\n" << endl;
	    return 1;
	} else if (debugLevel > 0) {
	    cerr << "dssi-vst-server[1]: registered Windows application class \"" << APPLICATION_CLASS_NAME << "\"" << endl;
	}
	
	hWnd = CreateWindow
	    (APPLICATION_CLASS_NAME, remoteVSTServerInstance->getName().c_str(),
	     WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
	     CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
	     0, 0, hInst, 0);
	if (!hWnd) {
	    cerr << "dssi-vst-server: ERROR: Failed to create window!\n" << endl;
	    return 1;
	} else if (debugLevel > 0) {
	    cerr << "dssi-vst-server[1]: created main window" << endl;
	}

	if (!haveGui) {
	    cerr << "Should be showing message here" << endl;
	} else {

	    plugin->dispatcher(plugin, effEditOpen, 0, 0, hWnd, 0);
	    Rect *rect = 0;
	    plugin->dispatcher(plugin, effEditGetRect, 0, 0, &rect, 0);
	    if (!rect) {
		cerr << "dssi-vst-server: ERROR: Plugin failed to report window size\n" << endl;
		return 1;
	    }

	    // Seems we need to provide space in here for the titlebar and frame,
	    // even though we don't know how big they'll be!  How crap.
	    SetWindowPos(hWnd, 0, 0, 0,
			 rect->right - rect->left + 6,
			 rect->bottom - rect->top + 25,
			 SWP_NOACTIVATE | SWP_NOMOVE |
			 SWP_NOOWNERZORDER | SWP_NOZORDER);
	    
	    if (debugLevel > 0) {
		cerr << "dssi-vst-server[1]: sized window" << endl;
	    }
	}

	ShowWindow(hWnd, SW_SHOWNORMAL);
	UpdateWindow(hWnd);

	if (debugLevel > 0) {
	    cerr << "dssi-vst-server[1]: showed window" << endl;
	}
    }

    cout << "done" << endl;

    DWORD threadId = 0;
    HANDLE threadHandle = CreateThread(0, 0, AudioThreadMain, 0, 0, &threadId);
    if (!threadHandle) {
	cerr << "Failed to create audio thread!" << endl;
	delete remoteVSTServerInstance;
	FreeLibrary(libHandle);
	return 1;
    } else if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: created audio thread" << endl;
    }

    ready = true;

    MSG msg;
    exiting = false;
    while (!exiting) {
	if (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
	    DispatchMessage(&msg);
	} else {
	    if (tryGui) {
		usleep(10000);
	    } else {
		sleep(1);
	    }
	}
    }

    if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: cleaning up" << endl;
    }

    CloseHandle(threadHandle);
    if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: closed audio thread" << endl;
    }

    delete remoteVSTServerInstance;

    FreeLibrary(libHandle);
    if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: freed dll" << endl;
    }

    if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: exiting" << endl;
    }

    return 0;
}

