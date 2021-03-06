#include "moonlight.hpp"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "ppapi/lib/gl/gles2/gl2ext_ppapi.h"

#include <h264_stream.h>

#define INITIAL_DECODE_BUFFER_LEN 128 * 1024

static unsigned char* s_DecodeBuffer;
static unsigned int s_DecodeBufferLength;
static int s_LastTextureType;
static int s_LastTextureId;
static int s_NextDecodeFrameNumber;
static int s_LastDisplayFrameNumber;
static unsigned char s_LastSps[256];
static unsigned char s_LastPps[256];
static unsigned int s_LastSpsLength;
static unsigned int s_LastPpsLength;

#define assertNoGLError() assert(!g_Instance->m_GlesApi->GetError(g_Instance->m_Graphics3D->pp_resource()))

static const char k_VertexShader[] =
    "varying vec2 v_texCoord;            \n"
    "attribute vec4 a_position;          \n"
    "attribute vec2 a_texCoord;          \n"
    "uniform vec2 v_scale;               \n"
    "void main()                         \n"
    "{                                   \n"
    "    v_texCoord = v_scale * a_texCoord; \n"
    "    gl_Position = a_position;       \n"
    "}";

static const char k_FragmentShader2D[] =
    "precision mediump float;            \n"
    "varying vec2 v_texCoord;            \n"
    "uniform sampler2D s_texture;        \n"
    "void main()                         \n"
    "{"
    "    gl_FragColor = texture2D(s_texture, v_texCoord); \n"
    "}";
    
static const char k_FragmentShaderRectangle[] =
    "#extension GL_ARB_texture_rectangle : require\n"
    "precision mediump float;            \n"
    "varying vec2 v_texCoord;            \n"
    "uniform sampler2DRect s_texture;    \n"
    "void main()                         \n"
    "{"
    "    gl_FragColor = texture2DRect(s_texture, v_texCoord).rgba; \n"
    "}";
    
static const char k_FragmentShaderExternal[] =
      "#extension GL_OES_EGL_image_external : require\n"
      "precision mediump float;            \n"
      "varying vec2 v_texCoord;            \n"
      "uniform samplerExternalOES s_texture; \n"
      "void main()                         \n"
      "{"
      "    gl_FragColor = texture2D(s_texture, v_texCoord); \n"
      "}";
    
void MoonlightInstance::DidChangeFocus(bool got_focus) {
    // Request an IDR frame to dump the frame queue that may have
    // built up from the GL pipeline being stalled.
    if (got_focus) {
        g_Instance->m_RequestIdrFrame = true;
    }
}

void MoonlightInstance::InitializeRenderingSurface(int width, int height) {
    if (!glInitializePPAPI(pp::Module::Get()->get_browser_interface())) {
        return;
    }
    
    int32_t contextAttributes[] = {
        PP_GRAPHICS3DATTRIB_BLUE_SIZE, 8,
        PP_GRAPHICS3DATTRIB_GREEN_SIZE, 8,
        PP_GRAPHICS3DATTRIB_RED_SIZE, 8,
        PP_GRAPHICS3DATTRIB_WIDTH, width,
        PP_GRAPHICS3DATTRIB_HEIGHT, height,
        PP_GRAPHICS3DATTRIB_NONE
    };
    g_Instance->m_Graphics3D = pp::Graphics3D(this, contextAttributes);
    
    int32_t swapBehaviorAttribute[] = {
        PP_GRAPHICS3DATTRIB_SWAP_BEHAVIOR, PP_GRAPHICS3DATTRIB_BUFFER_DESTROYED,
        PP_GRAPHICS3DATTRIB_NONE
    };
    g_Instance->m_Graphics3D.SetAttribs(swapBehaviorAttribute);
    
    if (!BindGraphics(m_Graphics3D)) {
      fprintf(stderr, "Unable to bind 3d context!\n");
      m_Graphics3D = pp::Graphics3D();
      glSetCurrentContextPPAPI(0);
      return;
    }
    
    glSetCurrentContextPPAPI(m_Graphics3D.pp_resource());
    
    glDisable(GL_DITHER);
    
    glViewport(0, 0, width, height);
    
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    assertNoGLError();
    
    static const float k_Vertices[] = {
        -1, -1, -1, 1, 1, -1, 1, 1,  // Position coordinates.
        0,  1,  0,  0, 1, 1,  1, 0,  // Texture coordinates.
    };

    GLuint buffer;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);

    glBufferData(GL_ARRAY_BUFFER,
                 sizeof(k_Vertices),
                 k_Vertices,
                 GL_STATIC_DRAW);
    assertNoGLError();
    
    g_Instance->m_Graphics3D.SwapBuffers(pp::BlockUntilComplete());
}

void MoonlightInstance::VidDecSetup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
    g_Instance->m_VideoDecoder = new pp::VideoDecoder(g_Instance);
    
    s_DecodeBufferLength = INITIAL_DECODE_BUFFER_LEN;
    s_DecodeBuffer = (unsigned char *)malloc(s_DecodeBufferLength);
    s_LastTextureType = 0;
    s_LastTextureId = 0;
    s_LastSpsLength = 0;
    s_LastPpsLength = 0;
    s_NextDecodeFrameNumber = 0;
    s_LastDisplayFrameNumber = 0;
    
    g_Instance->m_VideoDecoder->Initialize(g_Instance->m_Graphics3D,
                                           PP_VIDEOPROFILE_H264HIGH,
                                           PP_HARDWAREACCELERATION_ONLY,
                                           0,
                                           pp::BlockUntilComplete());
    
    pp::Module::Get()->core()->CallOnMainThread(0,
        g_Instance->m_CallbackFactory.NewCallback(&MoonlightInstance::DispatchGetPicture));
}

void MoonlightInstance::DispatchGetPicture(uint32_t unused) {
    // Queue the initial GetPicture callback on the main thread
    g_Instance->m_VideoDecoder->GetPicture(
        g_Instance->m_CallbackFactory.NewCallbackWithOutput(&MoonlightInstance::PictureReady));
}

void MoonlightInstance::VidDecCleanup(void) {
    free(s_DecodeBuffer);
    
    // Flush and delete the decoder
    g_Instance->m_VideoDecoder->Flush(pp::BlockUntilComplete());
    delete g_Instance->m_VideoDecoder;
    
    if (g_Instance->m_Texture2DShader.program) {
        glDeleteProgram(g_Instance->m_Texture2DShader.program);
        g_Instance->m_Texture2DShader.program = 0;
    }
    if (g_Instance->m_RectangleArbShader.program) {
        glDeleteProgram(g_Instance->m_RectangleArbShader.program);
        g_Instance->m_RectangleArbShader.program = 0;
    }
    if (g_Instance->m_ExternalOesShader.program) {
        glDeleteProgram(g_Instance->m_ExternalOesShader.program);
        g_Instance->m_ExternalOesShader.program = 0;
    }
}

static void ProcessSpsNalu(unsigned char* data, int length) {
    const char naluHeader[] = {0x00, 0x00, 0x00, 0x01};
    h264_stream_t* stream = h264_new();
    
    // Read the old NALU
    read_nal_unit(stream, &data[sizeof(naluHeader)], length-sizeof(naluHeader));
    
    // Fixup the SPS to what OS X needs to use hardware acceleration
    stream->sps->num_ref_frames = 1;
    stream->sps->vui.max_dec_frame_buffering = 1;
    
    // Copy the NALU prefix over from the original SPS
    memcpy(s_LastSps, naluHeader, sizeof(naluHeader));
    
    // Copy the modified NALU data
    s_LastSpsLength = sizeof(naluHeader) + write_nal_unit(stream,
                                                          &s_LastSps[sizeof(naluHeader)],
                                                          sizeof(s_LastSps)-sizeof(naluHeader));
    
    h264_free(stream);
}

int MoonlightInstance::VidDecSubmitDecodeUnit(PDECODE_UNIT decodeUnit) {
    PLENTRY entry;
    unsigned int offset;
    unsigned int totalLength;
    bool isSps = false;
    bool isPps = false;
    bool isIframe = false;
    
    // Request an IDR frame if needed
    if (g_Instance->m_RequestIdrFrame) {
        g_Instance->m_RequestIdrFrame = false;
        return DR_NEED_IDR;
    }
    
    // Look at the NALU type
    if (decodeUnit->bufferList->length > 5) {
        switch (decodeUnit->bufferList->data[4]) {
            case 0x67:
                isSps = true;
                
                // Store the SPS for later submission with the I-frame
                assert(decodeUnit->bufferList->length == decodeUnit->fullLength);
                assert(decodeUnit->fullLength < sizeof(s_LastSps));
                ProcessSpsNalu((unsigned char*)decodeUnit->bufferList->data,
                               decodeUnit->bufferList->length);
                
                // Don't submit anything to the decoder yet
                return DR_OK;
                
            case 0x68:
                isPps = true;
                
                // Store the PPS for later submission with the I-frame
                assert(decodeUnit->bufferList->length == decodeUnit->fullLength);
                assert(decodeUnit->fullLength < sizeof(s_LastPps));
                s_LastPpsLength = decodeUnit->bufferList->length;
                memcpy(s_LastPps, decodeUnit->bufferList->data, s_LastPpsLength);

                // Don't submit anything to the decoder yet
                return DR_OK;
                
            case 0x65:
                isIframe = true;
                break;
        }
    }
    
    // Chrome on OS X requires the SPS and PPS submitted together with
    // the first I-frame for hardware acceleration to work.
    totalLength = decodeUnit->fullLength;
    if (isIframe) {
        totalLength += s_LastSpsLength + s_LastPpsLength;
    }
    
    // Resize the decode buffer if needed
    if (totalLength > s_DecodeBufferLength) {
        free(s_DecodeBuffer);
        s_DecodeBufferLength = totalLength;
        s_DecodeBuffer = (unsigned char *)malloc(s_DecodeBufferLength);
    }
    
    if (isIframe) {
        // Copy the SPS and PPS in front of the frame data if this is an I-frame
        memcpy(&s_DecodeBuffer[0], s_LastSps, s_LastSpsLength);
        memcpy(&s_DecodeBuffer[s_LastSpsLength], s_LastPps, s_LastPpsLength);
        offset = s_LastSpsLength + s_LastPpsLength;
    }
    else {
        // Copy the frame data at the beginning of the buffer
        offset = 0;
    }
    
    entry = decodeUnit->bufferList;
    while (entry != NULL) {
        memcpy(&s_DecodeBuffer[offset], entry->data, entry->length);
        offset += entry->length;
        
        entry = entry->next;
    }
    
    // Start the decoding
    g_Instance->m_VideoDecoder->Decode(s_NextDecodeFrameNumber++, offset, s_DecodeBuffer, pp::BlockUntilComplete());
    
    return DR_OK;
}

void MoonlightInstance::CreateShader(GLuint program, GLenum type,
                                     const char* source, int size) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, &size);
    glCompileShader(shader);
    glAttachShader(program, shader);
    glDeleteShader(shader);
}

Shader MoonlightInstance::CreateProgram(const char* vertexShader, const char* fragmentShader) {
    Shader shader;
    
    shader.program = glCreateProgram();
    CreateShader(shader.program, GL_VERTEX_SHADER, vertexShader, strlen(vertexShader));
    CreateShader(shader.program, GL_FRAGMENT_SHADER, fragmentShader, strlen(fragmentShader));
    glLinkProgram(shader.program);
    glUseProgram(shader.program);
    
    glUniform1i(glGetUniformLocation(shader.program, "s_texture"), 0);
    assertNoGLError();
    
    shader.texcoord_scale_location = glGetUniformLocation(shader.program, "v_scale");
    
    GLint pos_location = glGetAttribLocation(shader.program, "a_position");
    GLint tc_location = glGetAttribLocation(shader.program, "a_texCoord");
    assertNoGLError();
    
    glEnableVertexAttribArray( pos_location);
    glVertexAttribPointer(pos_location, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(tc_location);
    glVertexAttribPointer(tc_location, 2, GL_FLOAT, GL_FALSE, 0, static_cast<float*>(0) + 8);
    
    glUseProgram(0);
    assertNoGLError();
    return shader;
}

void MoonlightInstance::PaintPicture(void) {
    m_IsPainting = true;
    
    // Free and skip all frames except the latest one
    PP_VideoPicture picture;
    while (m_PendingPictureQueue.size() > 1) {
        picture = m_PendingPictureQueue.front();
        m_PendingPictureQueue.pop();
        g_Instance->m_VideoDecoder->RecyclePicture(picture);
    }
    
    picture = m_PendingPictureQueue.front();
    
    // Recycle bogus pictures immediately
    if (picture.texture_target == 0) {
        g_Instance->m_VideoDecoder->RecyclePicture(picture);
        m_PendingPictureQueue.pop();
        m_IsPainting = false;
        return;
    }
    
    // Calling glClear() once per frame is recommended for modern
    // GPUs which use it for state tracking hints.
    glClear(GL_COLOR_BUFFER_BIT);
    
    int originalTextureTarget = s_LastTextureType;
    
    // Only make these state changes if we've changed from the last texture type
    if (picture.texture_target != s_LastTextureType) {
        if (picture.texture_target == GL_TEXTURE_2D) {
            if (!g_Instance->m_Texture2DShader.program) {
                g_Instance->m_Texture2DShader = CreateProgram(k_VertexShader, k_FragmentShader2D);
            }
            glUseProgram(g_Instance->m_Texture2DShader.program);
            glUniform2f(g_Instance->m_Texture2DShader.texcoord_scale_location, 1.0, 1.0);
        }
        else if (picture.texture_target == GL_TEXTURE_RECTANGLE_ARB) {
            if (!g_Instance->m_RectangleArbShader.program) {
                g_Instance->m_RectangleArbShader = CreateProgram(k_VertexShader, k_FragmentShaderRectangle);
            }
            glUseProgram(g_Instance->m_RectangleArbShader.program);
            glUniform2f(g_Instance->m_RectangleArbShader.texcoord_scale_location,
                        picture.texture_size.width, picture.texture_size.height);
        }
        else if (picture.texture_target == GL_TEXTURE_EXTERNAL_OES) {
            if (!g_Instance->m_ExternalOesShader.program) {
                g_Instance->m_ExternalOesShader = CreateProgram(k_VertexShader, k_FragmentShaderExternal);
            }
            glUseProgram(g_Instance->m_ExternalOesShader.program);
            glUniform2f(g_Instance->m_ExternalOesShader.texcoord_scale_location, 1.0, 1.0);
        }

        glActiveTexture(GL_TEXTURE0);

        s_LastTextureType = picture.texture_target;
    }
    
    // Only rebind our texture if we've changed since last time
    if (picture.texture_id != s_LastTextureId || picture.texture_target != originalTextureTarget) {
        glBindTexture(picture.texture_target, picture.texture_id);
        s_LastTextureId = picture.texture_id;
    }
    
    // Draw the image
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    
    // Swap buffers
    g_Instance->m_Graphics3D.SwapBuffers(
        g_Instance->m_CallbackFactory.NewCallback(&MoonlightInstance::PaintFinished));
}

void MoonlightInstance::PaintFinished(int32_t result) {
    m_IsPainting = false;
    
    // Recycle the picture now that it's been painted
    g_Instance->m_VideoDecoder->RecyclePicture(m_PendingPictureQueue.front());
    m_PendingPictureQueue.pop();
    
    // Keep painting if we still have frames
    if (!m_PendingPictureQueue.empty()) {
        PaintPicture();
    }
}

void MoonlightInstance::PictureReady(int32_t result, PP_VideoPicture picture) {
    if (result == PP_ERROR_ABORTED) {
        return;
    }
    
    // Ensure we only push newer frames onto the display queue
    if (picture.decode_id > s_LastDisplayFrameNumber) {
        m_PendingPictureQueue.push(picture);
        s_LastDisplayFrameNumber = picture.decode_id;
    }
    else {
        // This picture is older than the last one we displayed. Discard
        // it without displaying.
        g_Instance->m_VideoDecoder->RecyclePicture(picture);
    }
    
    g_Instance->m_VideoDecoder->GetPicture(
        g_Instance->m_CallbackFactory.NewCallbackWithOutput(&MoonlightInstance::PictureReady));
    
    if (!m_IsPainting && !m_PendingPictureQueue.empty()) {
        PaintPicture();
    }
}

DECODER_RENDERER_CALLBACKS MoonlightInstance::s_DrCallbacks = {
    MoonlightInstance::VidDecSetup,
    MoonlightInstance::VidDecCleanup,
    MoonlightInstance::VidDecSubmitDecodeUnit,
    CAPABILITY_SLICES_PER_FRAME(4)
};