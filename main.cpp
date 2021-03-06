#include "moonlight.hpp"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <pairing.h>

#include "ppapi/cpp/input_event.h"

// Requests the NaCl module to connection to the server specified after the :
#define MSG_START_REQUEST "startRequest"
// Requests the NaCl module stop streaming
#define MSG_STOP_REQUEST "stopRequest"
// Sent by the NaCl module when the stream has stopped whether user-requested or not
#define MSG_STREAM_TERMINATED "streamTerminated"

#define MSG_OPENURL "openUrl"

MoonlightInstance* g_Instance;

MoonlightInstance::~MoonlightInstance() {}

class MoonlightModule : public pp::Module {
    public:
        MoonlightModule() : pp::Module() {}
        virtual ~MoonlightModule() {}

        virtual pp::Instance* CreateInstance(PP_Instance instance) {
            return new MoonlightInstance(instance);
        }
};

void MoonlightInstance::OnConnectionStarted(uint32_t unused) {
    // Tell the front end
    pp::Var response("Connection Established");
    PostMessage(response);
    
    // Start receiving input events
    RequestInputEvents(PP_INPUTEVENT_CLASS_MOUSE);
    RequestFilteringInputEvents(PP_INPUTEVENT_CLASS_WHEEL | PP_INPUTEVENT_CLASS_KEYBOARD);
}

void MoonlightInstance::OnConnectionStopped(uint32_t error) {
    // Not running anymore
    m_Running = false;
    
    // Stop receiving input events
    ClearInputEventRequest(PP_INPUTEVENT_CLASS_MOUSE | PP_INPUTEVENT_CLASS_WHEEL | PP_INPUTEVENT_CLASS_KEYBOARD);
    
    // Unlock the mouse
    UnlockMouse();
    
    // Join threads
    pthread_join(m_ConnectionThread, NULL);
    pthread_join(m_GamepadThread, NULL);
    
    // Notify the JS code that the stream has ended
    pp::Var response(MSG_STREAM_TERMINATED);
    PostMessage(response);
}

void MoonlightInstance::StopConnection() {
    pthread_t t;
    
    // Stopping needs to happen in a separate thread to avoid a potential deadlock
    // caused by us getting a callback to the main thread while inside LiStopConnection.
    pthread_create(&t, NULL, MoonlightInstance::StopThreadFunc, NULL);
    
    // We'll need to call the listener ourselves since our connection terminated callback
    // won't be invoked for a manually requested termination.
    OnConnectionStopped(0);
}

void* MoonlightInstance::StopThreadFunc(void* context) {
    // Stop the connection
    LiStopConnection();
    return NULL;
}

void* MoonlightInstance::GamepadThreadFunc(void* context) {
    MoonlightInstance* me = (MoonlightInstance*)context;

    while (me->m_Running) {
        me->PollGamepads();
        
        // Poll every 10 ms
        usleep(10 * 1000);
    }
    
    return NULL;
}

void* MoonlightInstance::ConnectionThreadFunc(void* context) {
    MoonlightInstance* me = (MoonlightInstance*)context;
    int err;
    
    // Post a status update before we begin
    pp::Var response("Starting connection to " + me->m_Host);
    me->PostMessage(response);
    
    err = LiStartConnection(me->m_Host.c_str(),
                            &me->m_StreamConfig,
                            &MoonlightInstance::s_ClCallbacks,
                            &MoonlightInstance::s_DrCallbacks,
                            &MoonlightInstance::s_ArCallbacks,
                            NULL, 0,
                            me->m_ServerMajorVersion);
    if (err != 0) {
        // Notify the JS code that the stream has ended
        pp::Var response(MSG_STREAM_TERMINATED);
        me->PostMessage(response);
        return NULL;
    }
    
    // Set running state before starting connection-specific threads
    me->m_Running = true;
    
    pthread_create(&me->m_GamepadThread, NULL, MoonlightInstance::GamepadThreadFunc, me);
    
    return NULL;
}

// hook from javascript into the CPP code.
void MoonlightInstance::HandleMessage(const pp::Var& var_message) {
     // Ignore the message if it is not a string.
    if (!var_message.is_dictionary())
        return;
    
    pp::VarDictionary msg(var_message);
    int32_t callbackId = msg.Get("callbackId").AsInt();
    std::string method = msg.Get("method").AsString();
    pp::VarArray params(msg.Get("params"));
    
    if (strcmp(method.c_str(), MSG_START_REQUEST) == 0) {
        HandleStartStream(callbackId, params);
    } else if (strcmp(method.c_str(), MSG_STOP_REQUEST) == 0) {
        HandleStopStream(callbackId, params);
    } else if (strcmp(method.c_str(), MSG_OPENURL) == 0) {
        HandleOpenURL(callbackId, params);
    } else if (strcmp(method.c_str(), "httpInit") == 0) {
        NvHTTPInit(callbackId, params);
    } else if (strcmp(method.c_str(), "makeCert") == 0) {
        MakeCert(callbackId, params);
    } else if (strcmp(method.c_str(), "pair") == 0) {
        HandlePair(callbackId, params);
    } else {
        pp::Var response("Unhandled message received: " + method);
        PostMessage(response);
    }
}

void MoonlightInstance::HandleStartStream(int32_t callbackId, pp::VarArray args) {
    std::string host = args.Get(0).AsString();
    std::string width = args.Get(1).AsString();
    std::string height = args.Get(2).AsString();
    std::string fps = args.Get(3).AsString();
    std::string bitrate = args.Get(4).AsString();
    std::string serverMajorVersion = args.Get(5).AsString();
    
    pp::Var response("Setting stream width to: " + width);
    PostMessage(response);
    response = ("Setting stream height to: " + height);
    PostMessage(response);
    response = ("Setting stream fps to: " + fps);
    PostMessage(response);
    response = ("Setting stream host to: " + host);
    PostMessage(response);
    response = ("Setting stream bitrate to: " + bitrate);
    PostMessage(response);
    response = ("Setting server major version to: " + serverMajorVersion);
    PostMessage(response);
    
    // Populate the stream configuration
    m_StreamConfig.width = stoi(width);
    m_StreamConfig.height = stoi(height);
    m_StreamConfig.fps = stoi(fps);
    m_StreamConfig.bitrate = stoi(bitrate); // kilobits per second
    m_StreamConfig.packetSize = 1024;
    m_StreamConfig.streamingRemotely = 0;
    m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    
    m_ServerMajorVersion = stoi(serverMajorVersion);
    
    // Initialize the rendering surface before starting the connection
    InitializeRenderingSurface(m_StreamConfig.width, m_StreamConfig.height);

    // Store the host from the start message
    m_Host = host;
    
    // Start the worker thread to establish the connection
    pthread_create(&m_ConnectionThread, NULL, MoonlightInstance::ConnectionThreadFunc, this);
    
    pp::VarDictionary ret;
    ret.Set("callbackId", pp::Var(callbackId));
    ret.Set("type", pp::Var("resolve"));
    ret.Set("ret", pp::VarDictionary());
    PostMessage(ret);
}

void MoonlightInstance::HandleStopStream(int32_t callbackId, pp::VarArray args) {
    // Begin connection teardown
    StopConnection();
    
    pp::VarDictionary ret;
    ret.Set("callbackId", pp::Var(callbackId));
    ret.Set("type", pp::Var("resolve"));
    ret.Set("ret", pp::VarDictionary());
    PostMessage(ret);
}

void MoonlightInstance::HandleOpenURL(int32_t callbackId, pp::VarArray args) {
    std::string url = args.Get(0).AsString();
    
    openHttpThread.message_loop().PostWork(m_CallbackFactory.NewCallback(&MoonlightInstance::NvHTTPRequest, callbackId, url));
    
    PostMessage(pp::Var (url.c_str()));
}

void MoonlightInstance::HandlePair(int32_t callbackId, pp::VarArray args) {
     openHttpThread.message_loop().PostWork(m_CallbackFactory.NewCallback(&MoonlightInstance::PairCallback, callbackId, args));
}

void MoonlightInstance::PairCallback(int32_t /*result*/, int32_t callbackId, pp::VarArray args) {
    int err = gs_pair(atoi(args.Get(0).AsString().c_str()), args.Get(1).AsString().c_str(), args.Get(2).AsString().c_str());
    
    pp::VarDictionary ret;
    ret.Set("callbackId", pp::Var(callbackId));
    ret.Set("type", pp::Var("resolve"));
    ret.Set("ret", pp::Var(err));
    PostMessage(ret);
}

bool MoonlightInstance::Init(uint32_t argc,
                             const char* argn[],
                             const char* argv[]) {
    g_Instance = this;
    return true;
}

namespace pp {
Module* CreateModule() {
    return new MoonlightModule();
}
}  // namespace pp