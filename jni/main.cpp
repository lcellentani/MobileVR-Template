
#include <jni.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
//#include <pthread.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>

#if !defined( EGL_OPENGL_ES3_BIT_KHR )
#define EGL_OPENGL_ES3_BIT_KHR		0x0040
#endif

#if !defined( GL_EXT_multisampled_render_to_texture )
typedef void (GL_APIENTRY* PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC) (GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height);
typedef void (GL_APIENTRY* PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC) (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level, GLsizei samples);
#endif

#include <android/sensor.h>
#include <android/log.h>
#include <android/native_window_jni.h>	// for native window JNI
#include "../native_app_glue/android_native_app_glue.h"

#include "VrApi.h"
#include "VrApi_Helpers.h"
#include "VrApi_Android.h"

#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, "native-activity", __VA_ARGS__))
#define LOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, "native-activity", __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, "native-activity", __VA_ARGS__))

#define LOG_ACCELEROMETER false

static const int CPU_LEVEL = 2;
static const int GPU_LEVEL = 3;

//================================================================================
//
// OpenGL-ES Utility Functions
//
//================================================================================

#if 0
#define GL( func )		func; GLCheckErrors();
#else
#define GL( func )		func;
#endif

static const char * EglErrorString(const EGLint error)
{
	switch (error)
	{
	case EGL_SUCCESS:				return "EGL_SUCCESS";
	case EGL_NOT_INITIALIZED:		return "EGL_NOT_INITIALIZED";
	case EGL_BAD_ACCESS:			return "EGL_BAD_ACCESS";
	case EGL_BAD_ALLOC:				return "EGL_BAD_ALLOC";
	case EGL_BAD_ATTRIBUTE:			return "EGL_BAD_ATTRIBUTE";
	case EGL_BAD_CONTEXT:			return "EGL_BAD_CONTEXT";
	case EGL_BAD_CONFIG:			return "EGL_BAD_CONFIG";
	case EGL_BAD_CURRENT_SURFACE:	return "EGL_BAD_CURRENT_SURFACE";
	case EGL_BAD_DISPLAY:			return "EGL_BAD_DISPLAY";
	case EGL_BAD_SURFACE:			return "EGL_BAD_SURFACE";
	case EGL_BAD_MATCH:				return "EGL_BAD_MATCH";
	case EGL_BAD_PARAMETER:			return "EGL_BAD_PARAMETER";
	case EGL_BAD_NATIVE_PIXMAP:		return "EGL_BAD_NATIVE_PIXMAP";
	case EGL_BAD_NATIVE_WINDOW:		return "EGL_BAD_NATIVE_WINDOW";
	case EGL_CONTEXT_LOST:			return "EGL_CONTEXT_LOST";
	default:						return "unknown";
	}
}

static const char * GlErrorString(GLenum error)
{
	switch (error)
	{
	case GL_NO_ERROR:						return "GL_NO_ERROR";
	case GL_INVALID_ENUM:					return "GL_INVALID_ENUM";
	case GL_INVALID_VALUE:					return "GL_INVALID_VALUE";
	case GL_INVALID_OPERATION:				return "GL_INVALID_OPERATION";
	case GL_INVALID_FRAMEBUFFER_OPERATION:	return "GL_INVALID_FRAMEBUFFER_OPERATION";
	case GL_OUT_OF_MEMORY:					return "GL_OUT_OF_MEMORY";
	default: return "unknown";
	}
}

static const char * GlFrameBufferStatusString(GLenum status)
{
	switch (status)
	{
	case GL_FRAMEBUFFER_UNDEFINED:						return "GL_FRAMEBUFFER_UNDEFINED";
	case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:			return "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT";
	case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:	return "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT";
	case GL_FRAMEBUFFER_UNSUPPORTED:					return "GL_FRAMEBUFFER_UNSUPPORTED";
	case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE:			return "GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE";
	default:											return "unknown";
	}
}

static void GLCheckErrors()
{
	for (int i = 0; i < 10; i++)
	{
		const GLenum error = glGetError();
		if (error == GL_NO_ERROR)
		{
			break;
		}
		LOGE("GL error: %s", GlErrorString(error));
	}
}

//================================================================================
//
// OpenGL-ES Utility Functions
//
//================================================================================

typedef struct
{
	EGLint		MajorVersion;
	EGLint		MinorVersion;
	EGLDisplay	Display;
	EGLConfig	Config;
	EGLSurface	TinySurface;
	EGLSurface	MainSurface;
	EGLContext	Context;
} ovrEgl;

static void ovrEgl_Clear(ovrEgl * egl)
{
	egl->MajorVersion = 0;
	egl->MinorVersion = 0;
	egl->Display = 0;
	egl->Config = 0;
	egl->TinySurface = EGL_NO_SURFACE;
	egl->MainSurface = EGL_NO_SURFACE;
	egl->Context = EGL_NO_CONTEXT;
}

static void ovrEgl_CreateContext(ovrEgl * egl, const ovrEgl * shareEgl)
{
	if (egl->Display != 0)
	{
		return;
	}

	egl->Display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	LOGI("        eglInitialize( Display, &MajorVersion, &MinorVersion )");
	eglInitialize(egl->Display, &egl->MajorVersion, &egl->MinorVersion);
	// Do NOT use eglChooseConfig, because the Android EGL code pushes in multisample
	// flags in eglChooseConfig if the user has selected the "force 4x MSAA" option in
	// settings, and that is completely wasted for our warp target.
	const int MAX_CONFIGS = 1024;
	EGLConfig configs[MAX_CONFIGS];
	EGLint numConfigs = 0;
	if (eglGetConfigs(egl->Display, configs, MAX_CONFIGS, &numConfigs) == EGL_FALSE)
	{
		LOGE("        eglGetConfigs() failed: %s", EglErrorString(eglGetError()));
		return;
	}
	const EGLint configAttribs[] =
	{
		EGL_ALPHA_SIZE, 8, // need alpha for the multi-pass timewarp compositor
		EGL_BLUE_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_RED_SIZE, 8,
		EGL_DEPTH_SIZE, 0,
		EGL_SAMPLES, 0,
		EGL_NONE
	};
	egl->Config = 0;
	for (int i = 0; i < numConfigs; i++)
	{
		EGLint value = 0;

		eglGetConfigAttrib(egl->Display, configs[i], EGL_RENDERABLE_TYPE, &value);
		if ((value & EGL_OPENGL_ES3_BIT_KHR) != EGL_OPENGL_ES3_BIT_KHR)
		{
			continue;
		}

		// The pbuffer config also needs to be compatible with normal window rendering
		// so it can share textures with the window context.
		eglGetConfigAttrib(egl->Display, configs[i], EGL_SURFACE_TYPE, &value);
		if ((value & (EGL_WINDOW_BIT | EGL_PBUFFER_BIT)) != (EGL_WINDOW_BIT | EGL_PBUFFER_BIT))
		{
			continue;
		}

		int	j = 0;
		for (; configAttribs[j] != EGL_NONE; j += 2)
		{
			eglGetConfigAttrib(egl->Display, configs[i], configAttribs[j], &value);
			if (value != configAttribs[j + 1])
			{
				break;
			}
		}
		if (configAttribs[j] == EGL_NONE)
		{
			egl->Config = configs[i];
			break;
		}
	}
	if (egl->Config == 0)
	{
		LOGE("        eglChooseConfig() failed: %s", EglErrorString(eglGetError()));
		return;
	}
	EGLint contextAttribs[] =
	{
		EGL_CONTEXT_CLIENT_VERSION, 3,
		EGL_NONE
	};
	LOGI("        Context = eglCreateContext( Display, Config, EGL_NO_CONTEXT, contextAttribs )");
	egl->Context = eglCreateContext(egl->Display, egl->Config, (shareEgl != NULL) ? shareEgl->Context : EGL_NO_CONTEXT, contextAttribs);
	if (egl->Context == EGL_NO_CONTEXT)
	{
		LOGE("        eglCreateContext() failed: %s", EglErrorString(eglGetError()));
		return;
	}
	const EGLint surfaceAttribs[] =
	{
		EGL_WIDTH, 16,
		EGL_HEIGHT, 16,
		EGL_NONE
	};
	LOGI("        TinySurface = eglCreatePbufferSurface( Display, Config, surfaceAttribs )");
	egl->TinySurface = eglCreatePbufferSurface(egl->Display, egl->Config, surfaceAttribs);
	if (egl->TinySurface == EGL_NO_SURFACE)
	{
		LOGE("        eglCreatePbufferSurface() failed: %s", EglErrorString(eglGetError()));
		eglDestroyContext(egl->Display, egl->Context);
		egl->Context = EGL_NO_CONTEXT;
		return;
	}
	LOGI("        eglMakeCurrent( Display, TinySurface, TinySurface, Context )");
	if (eglMakeCurrent(egl->Display, egl->TinySurface, egl->TinySurface, egl->Context) == EGL_FALSE)
	{
		LOGE("        eglMakeCurrent() failed: %s", EglErrorString(eglGetError()));
		eglDestroySurface(egl->Display, egl->TinySurface);
		eglDestroyContext(egl->Display, egl->Context);
		egl->Context = EGL_NO_CONTEXT;
		return;
	}
}

static void ovrEgl_DestroyContext(ovrEgl * egl)
{
	if (egl->Display != 0)
	{
		LOGE("        eglMakeCurrent( Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT )");
		if (eglMakeCurrent(egl->Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_FALSE)
		{
			LOGE("        eglMakeCurrent() failed: %s", EglErrorString(eglGetError()));
		}
	}
	if (egl->Context != EGL_NO_CONTEXT)
	{
		LOGE("        eglDestroyContext( Display, Context )");
		if (eglDestroyContext(egl->Display, egl->Context) == EGL_FALSE)
		{
			LOGE("        eglDestroyContext() failed: %s", EglErrorString(eglGetError()));
		}
		egl->Context = EGL_NO_CONTEXT;
	}
	if (egl->TinySurface != EGL_NO_SURFACE)
	{
		LOGE("        eglDestroySurface( Display, TinySurface )");
		if (eglDestroySurface(egl->Display, egl->TinySurface) == EGL_FALSE)
		{
			LOGE("        eglDestroySurface() failed: %s", EglErrorString(eglGetError()));
		}
		egl->TinySurface = EGL_NO_SURFACE;
	}
	if (egl->Display != 0)
	{
		LOGE("        eglTerminate( Display )");
		if (eglTerminate(egl->Display) == EGL_FALSE)
		{
			LOGE("        eglTerminate() failed: %s", EglErrorString(eglGetError()));
		}
		egl->Display = 0;
	}
}

static void ovrEgl_CreateSurface(ovrEgl * egl, ANativeWindow * nativeWindow)
{
	if (egl->MainSurface != EGL_NO_SURFACE)
	{
		return;
	}
	LOGI("        MainSurface = eglCreateWindowSurface( Display, Config, nativeWindow, attribs )");
	const EGLint surfaceAttribs[] = { EGL_NONE };
	egl->MainSurface = eglCreateWindowSurface(egl->Display, egl->Config, nativeWindow, surfaceAttribs);
	if (egl->MainSurface == EGL_NO_SURFACE)
	{
		LOGE("        eglCreateWindowSurface() failed: %s", EglErrorString(eglGetError()));
		return;
	}
	LOGI("        eglMakeCurrent( display, MainSurface, MainSurface, Context )");
	if (eglMakeCurrent(egl->Display, egl->MainSurface, egl->MainSurface, egl->Context) == EGL_FALSE)
	{
		LOGE("        eglMakeCurrent() failed: %s", EglErrorString(eglGetError()));
		return;
	}
}

static void ovrEgl_DestroySurface(ovrEgl * egl)
{
	if (egl->Context != EGL_NO_CONTEXT)
	{
		LOGI("        eglMakeCurrent( display, TinySurface, TinySurface, Context )");
		if (eglMakeCurrent(egl->Display, egl->TinySurface, egl->TinySurface, egl->Context) == EGL_FALSE)
		{
			LOGE("        eglMakeCurrent() failed: %s", EglErrorString(eglGetError()));
		}
	}
	if (egl->MainSurface != EGL_NO_SURFACE)
	{
		LOGI("        eglDestroySurface( Display, MainSurface )");
		if (eglDestroySurface(egl->Display, egl->MainSurface) == EGL_FALSE)
		{
			LOGE("        eglDestroySurface() failed: %s", EglErrorString(eglGetError()));
		}
		egl->MainSurface = EGL_NO_SURFACE;
	}
}

//================================================================================
//
// ovrGeometry
//
//================================================================================

typedef struct
{
	GLuint			Index;
	GLint			Size;
	GLenum			Type;
	GLboolean		Normalized;
	GLsizei			Stride;
	const GLvoid *	Pointer;
} ovrVertexAttribPointer;

#define MAX_VERTEX_ATTRIB_POINTERS		3

typedef struct
{
	GLuint					VertexBuffer;
	GLuint					IndexBuffer;
	GLuint					VertexArrayObject;
	int						VertexCount;
	int 					IndexCount;
	ovrVertexAttribPointer	VertexAttribs[MAX_VERTEX_ATTRIB_POINTERS];
} ovrGeometry;

enum
{
	VERTEX_ATTRIBUTE_LOCATION_POSITION,
	VERTEX_ATTRIBUTE_LOCATION_COLOR,
	VERTEX_ATTRIBUTE_LOCATION_UV,
	VERTEX_ATTRIBUTE_LOCATION_TRANSFORM
};

typedef struct
{
	int				location;
	const char *	name;
} ovrVertexAttribute;

static ovrVertexAttribute ProgramVertexAttributes[] =
{
	{ VERTEX_ATTRIBUTE_LOCATION_POSITION, "vertexPosition" },
	{ VERTEX_ATTRIBUTE_LOCATION_COLOR, "vertexColor" },
	{ VERTEX_ATTRIBUTE_LOCATION_UV, "vertexUv" },
	{ VERTEX_ATTRIBUTE_LOCATION_TRANSFORM, "vertexTransform" }
};

static void ovrGeometry_Clear(ovrGeometry * geometry)
{
	geometry->VertexBuffer = 0;
	geometry->IndexBuffer = 0;
	geometry->VertexArrayObject = 0;
	geometry->VertexCount = 0;
	geometry->IndexCount = 0;
	for (int i = 0; i < MAX_VERTEX_ATTRIB_POINTERS; i++)
	{
		memset(&geometry->VertexAttribs[i], 0, sizeof(geometry->VertexAttribs[i]));
		geometry->VertexAttribs[i].Index = -1;
	}
}

#ifndef offsetof
#define offsetof(st, m) ((size_t)(&((st *)0)->m))
#endif

static void ovrGeometry_CreateCube(ovrGeometry * geometry)
{
	typedef struct
	{
		char positions[8][4];
		unsigned char colors[8][4];
	} ovrCubeVertices;

	static const ovrCubeVertices cubeVertices =
	{
		// positions
		{
			{ -127, +127, -127, +127 }, { +127, +127, -127, +127 }, { +127, +127, +127, +127 }, { -127, +127, +127, +127 },	// top
			{ -127, -127, -127, +127 }, { -127, -127, +127, +127 }, { +127, -127, +127, +127 }, { +127, -127, -127, +127 }	// bottom
		},
		// colors
		{
			{ 255, 0, 255, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 0, 0, 255 },
			{ 0, 0, 255, 255 }, { 0, 255, 0, 255 }, { 255, 0, 255, 255 }, { 255, 0, 0, 255 }
		},
	};

	static const unsigned short cubeIndices[36] =
	{
		0, 1, 2, 2, 3, 0,	// top
		4, 5, 6, 6, 7, 4,	// bottom
		2, 6, 7, 7, 1, 2,	// right
		0, 4, 5, 5, 3, 0,	// left
		3, 5, 6, 6, 2, 3,	// front
		0, 1, 7, 7, 4, 0	// back
	};

	geometry->VertexCount = 8;
	geometry->IndexCount = 36;

	geometry->VertexAttribs[0].Index = VERTEX_ATTRIBUTE_LOCATION_POSITION;
	geometry->VertexAttribs[0].Size = 4;
	geometry->VertexAttribs[0].Type = GL_BYTE;
	geometry->VertexAttribs[0].Normalized = true;
	geometry->VertexAttribs[0].Stride = sizeof(cubeVertices.positions[0]);
	geometry->VertexAttribs[0].Pointer = (const GLvoid *)offsetof(ovrCubeVertices, positions);//(const GLvoid *)((size_t)&(cubeVertices.positions));

	geometry->VertexAttribs[1].Index = VERTEX_ATTRIBUTE_LOCATION_COLOR;
	geometry->VertexAttribs[1].Size = 4;
	geometry->VertexAttribs[1].Type = GL_UNSIGNED_BYTE;
	geometry->VertexAttribs[1].Normalized = true;
	geometry->VertexAttribs[1].Stride = sizeof(cubeVertices.colors[0]);
	geometry->VertexAttribs[1].Pointer = (const GLvoid *)offsetof(ovrCubeVertices, colors);//(const GLvoid *)((size_t)&(cubeVertices.colors));

	GL(glGenBuffers(1, &geometry->VertexBuffer));
	GL(glBindBuffer(GL_ARRAY_BUFFER, geometry->VertexBuffer));
	GL(glBufferData(GL_ARRAY_BUFFER, sizeof(cubeVertices), &cubeVertices, GL_STATIC_DRAW));
	GL(glBindBuffer(GL_ARRAY_BUFFER, 0));

	GL(glGenBuffers(1, &geometry->IndexBuffer));
	GL(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->IndexBuffer));
	GL(glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(cubeIndices), cubeIndices, GL_STATIC_DRAW));
	GL(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
}

static void ovrGeometry_Destroy(ovrGeometry * geometry)
{
	GL(glDeleteBuffers(1, &geometry->IndexBuffer));
	GL(glDeleteBuffers(1, &geometry->VertexBuffer));

	ovrGeometry_Clear(geometry);
}

static void ovrGeometry_CreateVAO(ovrGeometry * geometry)
{
	GL(glGenVertexArrays(1, &geometry->VertexArrayObject));
	GL(glBindVertexArray(geometry->VertexArrayObject));

	GL(glBindBuffer(GL_ARRAY_BUFFER, geometry->VertexBuffer));

	for (int i = 0; i < MAX_VERTEX_ATTRIB_POINTERS; i++)
	{
		if (geometry->VertexAttribs[i].Index != -1)
		{
			GL(glEnableVertexAttribArray(geometry->VertexAttribs[i].Index));
			GL(glVertexAttribPointer(geometry->VertexAttribs[i].Index, geometry->VertexAttribs[i].Size,
				geometry->VertexAttribs[i].Type, geometry->VertexAttribs[i].Normalized,
				geometry->VertexAttribs[i].Stride, geometry->VertexAttribs[i].Pointer));
		}
	}

	GL(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->IndexBuffer));

	GL(glBindVertexArray(0));
}

static void ovrGeometry_DestroyVAO(ovrGeometry * geometry)
{
	GL(glDeleteVertexArrays(1, &geometry->VertexArrayObject));
}

//================================================================================
//
// ovrProgram
//
//================================================================================

#define MAX_PROGRAM_UNIFORMS	8
#define MAX_PROGRAM_TEXTURES	8

typedef struct
{
	GLuint	Program;
	GLuint	VertexShader;
	GLuint	FragmentShader;
	// These will be -1 if not used by the program.
	GLint	Uniforms[MAX_PROGRAM_UNIFORMS];		// ProgramUniforms[].name
	GLint	Textures[MAX_PROGRAM_TEXTURES];		// Texture%i
} ovrProgram;

enum
{
	UNIFORM_MODEL_MATRIX,
	UNIFORM_VIEW_MATRIX,
	UNIFORM_PROJECTION_MATRIX
};

enum
{
	UNIFORM_TYPE_VECTOR4,
	UNIFORM_TYPE_MATRIX4X4
};

typedef struct
{
	int				index;
	int				type;
	const char *	name;
} ovrUniform;

static ovrUniform ProgramUniforms[] =
{
	{ UNIFORM_MODEL_MATRIX, UNIFORM_TYPE_MATRIX4X4, "ModelMatrix" },
	{ UNIFORM_VIEW_MATRIX, UNIFORM_TYPE_MATRIX4X4, "ViewMatrix" },
	{ UNIFORM_PROJECTION_MATRIX, UNIFORM_TYPE_MATRIX4X4, "ProjectionMatrix" }
};

static void ovrProgram_Clear(ovrProgram * program)
{
	program->Program = 0;
	program->VertexShader = 0;
	program->FragmentShader = 0;
	memset(program->Uniforms, 0, sizeof(program->Uniforms));
	memset(program->Textures, 0, sizeof(program->Textures));
}

static bool ovrProgram_Create(ovrProgram * program, const char * vertexSource, const char * fragmentSource)
{
	GLint r;

	GL(program->VertexShader = glCreateShader(GL_VERTEX_SHADER));
	GL(glShaderSource(program->VertexShader, 1, &vertexSource, 0));
	GL(glCompileShader(program->VertexShader));
	GL(glGetShaderiv(program->VertexShader, GL_COMPILE_STATUS, &r));
	if (r == GL_FALSE)
	{
		GLchar msg[4096];
		GL(glGetShaderInfoLog(program->VertexShader, sizeof(msg), 0, msg));
		LOGE("%s\n%s\n", vertexSource, msg);
		return false;
	}

	GL(program->FragmentShader = glCreateShader(GL_FRAGMENT_SHADER));
	GL(glShaderSource(program->FragmentShader, 1, &fragmentSource, 0));
	GL(glCompileShader(program->FragmentShader));
	GL(glGetShaderiv(program->FragmentShader, GL_COMPILE_STATUS, &r));
	if (r == GL_FALSE)
	{
		GLchar msg[4096];
		GL(glGetShaderInfoLog(program->FragmentShader, sizeof(msg), 0, msg));
		LOGE("%s\n%s\n", fragmentSource, msg);
		return false;
	}

	GL(program->Program = glCreateProgram());
	GL(glAttachShader(program->Program, program->VertexShader));
	GL(glAttachShader(program->Program, program->FragmentShader));

	// Bind the vertex attribute locations.
	for (int i = 0; i < sizeof(ProgramVertexAttributes) / sizeof(ProgramVertexAttributes[0]); i++)
	{
		GL(glBindAttribLocation(program->Program, ProgramVertexAttributes[i].location, ProgramVertexAttributes[i].name));
	}

	GL(glLinkProgram(program->Program));
	GL(glGetProgramiv(program->Program, GL_LINK_STATUS, &r));
	if (r == GL_FALSE)
	{
		GLchar msg[4096];
		GL(glGetProgramInfoLog(program->Program, sizeof(msg), 0, msg));
		LOGE("Linking program failed: %s\n", msg);
		return false;
	}

	// Get the uniform locations.
	memset(program->Uniforms, -1, sizeof(program->Uniforms));
	for (int i = 0; i < sizeof(ProgramUniforms) / sizeof(ProgramUniforms[0]); i++)
	{
		program->Uniforms[ProgramUniforms[i].index] = glGetUniformLocation(program->Program, ProgramUniforms[i].name);
	}

	GL(glUseProgram(program->Program));

	// Get the texture locations.
	for (int i = 0; i < MAX_PROGRAM_TEXTURES; i++)
	{
		char name[32];
		sprintf(name, "Texture%i", i);
		program->Textures[i] = glGetUniformLocation(program->Program, name);
		if (program->Textures[i] != -1)
		{
			GL(glUniform1i(program->Textures[i], i));
		}
	}

	GL(glUseProgram(0));

	return true;
}

static void ovrProgram_Destroy(ovrProgram * program)
{
	if (program->Program != 0)
	{
		GL(glDeleteProgram(program->Program));
		program->Program = 0;
	}
	if (program->VertexShader != 0)
	{
		GL(glDeleteShader(program->VertexShader));
		program->VertexShader = 0;
	}
	if (program->FragmentShader != 0)
	{
		GL(glDeleteShader(program->FragmentShader));
		program->FragmentShader = 0;
	}
}

//================================================================================
//
// ovrFramebuffer
//
//================================================================================

typedef struct
{
	int						Width;
	int						Height;
	int						Multisamples;
	int						TextureSwapChainLength;
	int						TextureSwapChainIndex;
	ovrTextureSwapChain *	ColorTextureSwapChain;
	GLuint *				DepthBuffers;
	GLuint *				FrameBuffers;
} ovrFramebuffer;

static void ovrFramebuffer_Clear(ovrFramebuffer * frameBuffer)
{
	frameBuffer->Width = 0;
	frameBuffer->Height = 0;
	frameBuffer->Multisamples = 0;
	frameBuffer->TextureSwapChainLength = 0;
	frameBuffer->TextureSwapChainIndex = 0;
	frameBuffer->ColorTextureSwapChain = NULL;
	frameBuffer->DepthBuffers = NULL;
	frameBuffer->FrameBuffers = NULL;
}

static bool ovrFramebuffer_Create(ovrFramebuffer * frameBuffer, const ovrTextureFormat colorFormat, const int width, const int height, const int multisamples)
{
	frameBuffer->Width = width;
	frameBuffer->Height = height;
	frameBuffer->Multisamples = multisamples;

	frameBuffer->ColorTextureSwapChain = vrapi_CreateTextureSwapChain(VRAPI_TEXTURE_TYPE_2D, colorFormat, width, height, 1, true);
	frameBuffer->TextureSwapChainLength = vrapi_GetTextureSwapChainLength(frameBuffer->ColorTextureSwapChain);
	frameBuffer->DepthBuffers = (GLuint *)malloc(frameBuffer->TextureSwapChainLength * sizeof(GLuint));
	frameBuffer->FrameBuffers = (GLuint *)malloc(frameBuffer->TextureSwapChainLength * sizeof(GLuint));

	PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC glRenderbufferStorageMultisampleEXT =
		(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC)eglGetProcAddress("glRenderbufferStorageMultisampleEXT");
	PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC glFramebufferTexture2DMultisampleEXT =
		(PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC)eglGetProcAddress("glFramebufferTexture2DMultisampleEXT");

	for (int i = 0; i < frameBuffer->TextureSwapChainLength; i++)
	{
		// Create the color buffer texture.
		const GLuint colorTexture = vrapi_GetTextureSwapChainHandle(frameBuffer->ColorTextureSwapChain, i);
		GL(glBindTexture(GL_TEXTURE_2D, colorTexture));
		GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
		GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
		GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
		GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
		GL(glBindTexture(GL_TEXTURE_2D, 0));

		if (multisamples > 1 && glRenderbufferStorageMultisampleEXT != NULL && glFramebufferTexture2DMultisampleEXT != NULL)
		{
			// Create multisampled depth buffer.
			GL(glGenRenderbuffers(1, &frameBuffer->DepthBuffers[i]));
			GL(glBindRenderbuffer(GL_RENDERBUFFER, frameBuffer->DepthBuffers[i]));
			GL(glRenderbufferStorageMultisampleEXT(GL_RENDERBUFFER, multisamples, GL_DEPTH_COMPONENT24, width, height));
			GL(glBindRenderbuffer(GL_RENDERBUFFER, 0));

			// Create the frame buffer.
			GL(glGenFramebuffers(1, &frameBuffer->FrameBuffers[i]));
			GL(glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer->FrameBuffers[i]));
			GL(glFramebufferTexture2DMultisampleEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTexture, 0, multisamples));
			GL(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, frameBuffer->DepthBuffers[i]));
			GL(GLenum renderFramebufferStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER));
			GL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
			if (renderFramebufferStatus != GL_FRAMEBUFFER_COMPLETE)
			{
				LOGE("Incomplete frame buffer object: %s", GlFrameBufferStatusString(renderFramebufferStatus));
				return false;
			}
		}
		else
		{
			// Create depth buffer.
			GL(glGenRenderbuffers(1, &frameBuffer->DepthBuffers[i]));
			GL(glBindRenderbuffer(GL_RENDERBUFFER, frameBuffer->DepthBuffers[i]));
			GL(glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height));
			GL(glBindRenderbuffer(GL_RENDERBUFFER, 0));

			// Create the frame buffer.
			GL(glGenFramebuffers(1, &frameBuffer->FrameBuffers[i]));
			GL(glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer->FrameBuffers[i]));
			GL(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, frameBuffer->DepthBuffers[i]));
			GL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTexture, 0));
			GL(GLenum renderFramebufferStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER));
			GL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
			if (renderFramebufferStatus != GL_FRAMEBUFFER_COMPLETE)
			{
				LOGE("Incomplete frame buffer object: %s", GlFrameBufferStatusString(renderFramebufferStatus));
				return false;
			}
		}
	}

	return true;
}

static void ovrFramebuffer_Destroy(ovrFramebuffer * frameBuffer)
{
	GL(glDeleteFramebuffers(frameBuffer->TextureSwapChainLength, frameBuffer->FrameBuffers));
	GL(glDeleteRenderbuffers(frameBuffer->TextureSwapChainLength, frameBuffer->DepthBuffers));
	vrapi_DestroyTextureSwapChain(frameBuffer->ColorTextureSwapChain);

	free(frameBuffer->DepthBuffers);
	free(frameBuffer->FrameBuffers);

	ovrFramebuffer_Clear(frameBuffer);
}

static void ovrFramebuffer_SetCurrent(ovrFramebuffer * frameBuffer)
{
	GL(glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer->FrameBuffers[frameBuffer->TextureSwapChainIndex]));
}

static void ovrFramebuffer_SetNone()
{
	GL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
}

static void ovrFramebuffer_Resolve(ovrFramebuffer * frameBuffer)
{
	// Discard the depth buffer, so the tiler won't need to write it back out to memory.
	const GLenum depthAttachment[1] = { GL_DEPTH_ATTACHMENT };
	glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, depthAttachment);

	// Flush this frame worth of commands.
	glFlush();
}

static void ovrFramebuffer_Advance(ovrFramebuffer * frameBuffer)
{
	// Advance to the next texture from the set.
	frameBuffer->TextureSwapChainIndex = (frameBuffer->TextureSwapChainIndex + 1) % frameBuffer->TextureSwapChainLength;
}

/*
================================================================================

ovrScene

================================================================================
*/

#define NUM_INSTANCES		1500

typedef struct
{
	bool				CreatedScene;
	bool				CreatedVAOs;
	unsigned int		Random;
	ovrProgram			Program;
	ovrGeometry			Cube;
	GLuint				InstanceTransformBuffer;
	ovrVector3f			CubePositions[NUM_INSTANCES];
	ovrVector3f			CubeRotations[NUM_INSTANCES];
} ovrScene;

static const char VERTEX_SHADER[] =
"#version 300 es\n"
"in vec3 vertexPosition;\n"
"in vec4 vertexColor;\n"
"in mat4 vertexTransform;\n"
"uniform mat4 ViewMatrix;\n"
"uniform mat4 ProjectionMatrix;\n"
"out vec4 fragmentColor;\n"
"void main()\n"
"{\n"
"	gl_Position = ProjectionMatrix * ( ViewMatrix * ( vertexTransform * vec4( vertexPosition, 1.0 ) ) );\n"
"	fragmentColor = vertexColor;\n"
"}\n";

static const char FRAGMENT_SHADER[] =
"#version 300 es\n"
"in lowp vec4 fragmentColor;\n"
"out lowp vec4 outColor;\n"
"void main()\n"
"{\n"
"	outColor = fragmentColor;\n"
"}\n";

static void ovrScene_Clear(ovrScene * scene)
{
	scene->CreatedScene = false;
	scene->CreatedVAOs = false;
	scene->Random = 2;
	scene->InstanceTransformBuffer = 0;

	ovrProgram_Clear(&scene->Program);
	ovrGeometry_Clear(&scene->Cube);
}

static bool ovrScene_IsCreated(ovrScene * scene)
{
	return scene->CreatedScene;
}

static void ovrScene_CreateVAOs(ovrScene * scene)
{
	if (!scene->CreatedVAOs)
	{
		ovrGeometry_CreateVAO(&scene->Cube);

		// Modify the VAO to use the instance transform attributes.
		GL(glBindVertexArray(scene->Cube.VertexArrayObject));
		GL(glBindBuffer(GL_ARRAY_BUFFER, scene->InstanceTransformBuffer));
		for (int i = 0; i < 4; i++)
		{
			GL(glEnableVertexAttribArray(VERTEX_ATTRIBUTE_LOCATION_TRANSFORM + i));
			GL(glVertexAttribPointer(VERTEX_ATTRIBUTE_LOCATION_TRANSFORM + i, 4, GL_FLOAT,
				false, 4 * 4 * sizeof(float), (void *)(i * 4 * sizeof(float))));
			GL(glVertexAttribDivisor(VERTEX_ATTRIBUTE_LOCATION_TRANSFORM + i, 1));
		}
		GL(glBindVertexArray(0));

		scene->CreatedVAOs = true;
	}
}

static void ovrScene_DestroyVAOs(ovrScene * scene)
{
	if (scene->CreatedVAOs)
	{
		ovrGeometry_DestroyVAO(&scene->Cube);

		scene->CreatedVAOs = false;
	}
}

static float ovrScene_RandomFloat(ovrScene * scene)
{
	scene->Random = 1664525L * scene->Random + 1013904223L;
	unsigned int rf = 0x3F800000 | (scene->Random & 0x007FFFFF);
	return (*(float *)&rf) - 1.0f;
}

static void ovrScene_Create(ovrScene * scene)
{
	ovrProgram_Create(&scene->Program, VERTEX_SHADER, FRAGMENT_SHADER);
	ovrGeometry_CreateCube(&scene->Cube);

	// Create the instance transform attribute buffer.
	GL(glGenBuffers(1, &scene->InstanceTransformBuffer));
	GL(glBindBuffer(GL_ARRAY_BUFFER, scene->InstanceTransformBuffer));
	GL(glBufferData(GL_ARRAY_BUFFER, NUM_INSTANCES * 4 * 4 * sizeof(float), NULL, GL_DYNAMIC_DRAW));
	GL(glBindBuffer(GL_ARRAY_BUFFER, 0));

	// Setup random cube positions and rotations.
	for (int i = 0; i < NUM_INSTANCES; i++)
	{
		// Using volatile keeps the compiler from optimizing away multiple calls to drand48().
		volatile float rx, ry, rz;
		for (;;)
		{
			rx = (ovrScene_RandomFloat(scene) - 0.5f) * (50.0f + sqrt(NUM_INSTANCES));
			ry = (ovrScene_RandomFloat(scene) - 0.5f) * (50.0f + sqrt(NUM_INSTANCES));
			rz = (ovrScene_RandomFloat(scene) - 0.5f) * (50.0f + sqrt(NUM_INSTANCES));
			// If too close to 0,0,0
			if (fabsf(rx) < 4.0f && fabsf(ry) < 4.0f && fabsf(rz) < 4.0f)
			{
				continue;
			}
			// Test for overlap with any of the existing cubes.
			bool overlap = false;
			for (int j = 0; j < i; j++)
			{
				if (fabsf(rx - scene->CubePositions[j].x) < 4.0f &&
					fabsf(ry - scene->CubePositions[j].y) < 4.0f &&
					fabsf(rz - scene->CubePositions[j].z) < 4.0f)
				{
					overlap = true;
					break;
				}
			}
			if (!overlap)
			{
				break;
			}
		}

		// Insert into list sorted based on distance.
		int insert = 0;
		const float distSqr = rx * rx + ry * ry + rz * rz;
		for (int j = i; j > 0; j--)
		{
			const ovrVector3f * otherPos = &scene->CubePositions[j - 1];
			const float otherDistSqr = otherPos->x * otherPos->x + otherPos->y * otherPos->y + otherPos->z * otherPos->z;
			if (distSqr > otherDistSqr)
			{
				insert = j;
				break;
			}
			scene->CubePositions[j] = scene->CubePositions[j - 1];
			scene->CubeRotations[j] = scene->CubeRotations[j - 1];
		}

		scene->CubePositions[insert].x = rx;
		scene->CubePositions[insert].y = ry;
		scene->CubePositions[insert].z = rz;

		scene->CubeRotations[insert].x = ovrScene_RandomFloat(scene);
		scene->CubeRotations[insert].y = ovrScene_RandomFloat(scene);
		scene->CubeRotations[insert].z = ovrScene_RandomFloat(scene);
	}

	scene->CreatedScene = true;

#if !MULTI_THREADED
	ovrScene_CreateVAOs(scene);
#endif
}

static void ovrScene_Destroy(ovrScene * scene)
{
#if !MULTI_THREADED
	ovrScene_DestroyVAOs(scene);
#endif

	ovrProgram_Destroy(&scene->Program);
	ovrGeometry_Destroy(&scene->Cube);
	GL(glDeleteBuffers(1, &scene->InstanceTransformBuffer));
	scene->CreatedScene = false;
}

//================================================================================
//
// ovrSimulation
//
//================================================================================

typedef struct
{
	ovrVector3f			CurrentRotation;
} ovrSimulation;

static void ovrSimulation_Clear(ovrSimulation * simulation)
{
	simulation->CurrentRotation.x = 0.0f;
	simulation->CurrentRotation.y = 0.0f;
	simulation->CurrentRotation.z = 0.0f;
}

static void ovrSimulation_Advance(ovrSimulation * simulation, double predictedDisplayTime)
{
	// Update rotation.
	simulation->CurrentRotation.x = (float)(predictedDisplayTime);
	simulation->CurrentRotation.y = (float)(predictedDisplayTime);
	simulation->CurrentRotation.z = (float)(predictedDisplayTime);
}

//================================================================================
//
// ovrRenderer
//
//================================================================================

#define NUM_MULTI_SAMPLES	4

typedef struct
{
	ovrFramebuffer	FrameBuffer[VRAPI_FRAME_LAYER_EYE_MAX];
	ovrMatrix4f		ProjectionMatrix;
	ovrMatrix4f		TexCoordsTanAnglesMatrix;
} ovrRenderer;

static void ovrRenderer_Clear(ovrRenderer * renderer)
{
	for (int eye = 0; eye < VRAPI_FRAME_LAYER_EYE_MAX; eye++)
	{
		ovrFramebuffer_Clear(&renderer->FrameBuffer[eye]);
	}
	renderer->ProjectionMatrix = ovrMatrix4f_CreateIdentity();
	renderer->TexCoordsTanAnglesMatrix = ovrMatrix4f_CreateIdentity();
}

static void ovrRenderer_Create(ovrRenderer * renderer, const ovrHmdInfo * hmdInfo)
{
	// Create the frame buffers.
	for (int eye = 0; eye < VRAPI_FRAME_LAYER_EYE_MAX; eye++)
	{
		ovrFramebuffer_Create(&renderer->FrameBuffer[eye],
			VRAPI_TEXTURE_FORMAT_8888,
			hmdInfo->SuggestedEyeResolutionWidth,
			hmdInfo->SuggestedEyeResolutionHeight,
			NUM_MULTI_SAMPLES);
	}

	// Setup the projection matrix.
	renderer->ProjectionMatrix = ovrMatrix4f_CreateProjectionFov(
		hmdInfo->SuggestedEyeFovDegreesX,
		hmdInfo->SuggestedEyeFovDegreesY,
		0.0f, 0.0f, 1.0f, 0.0f);
	renderer->TexCoordsTanAnglesMatrix = ovrMatrix4f_TanAngleMatrixFromProjection(&renderer->ProjectionMatrix);
}

static void ovrRenderer_Destroy(ovrRenderer * renderer)
{
	for (int eye = 0; eye < VRAPI_FRAME_LAYER_EYE_MAX; eye++)
	{
		ovrFramebuffer_Destroy(&renderer->FrameBuffer[eye]);
	}
	renderer->ProjectionMatrix = ovrMatrix4f_CreateIdentity();
	renderer->TexCoordsTanAnglesMatrix = ovrMatrix4f_CreateIdentity();
}

static ovrFrameParms ovrRenderer_RenderFrame(ovrRenderer * renderer, const ovrJava * java,
	long long frameIndex, int minimumVsyncs, const ovrPerformanceParms * perfParms,
	const ovrScene * scene, const ovrSimulation * simulation,
	const ovrTracking * tracking, ovrMobile * ovr)
{
	ovrFrameParms parms = vrapi_DefaultFrameParms(java, VRAPI_FRAME_INIT_DEFAULT, NULL);
	parms.FrameIndex = frameIndex;
	parms.MinimumVsyncs = minimumVsyncs;
	parms.PerformanceParms = *perfParms;

	// Update the instance transform attributes.
	GL(glBindBuffer(GL_ARRAY_BUFFER, scene->InstanceTransformBuffer));
	GL(ovrMatrix4f * cubeTransforms = (ovrMatrix4f *)glMapBufferRange(GL_ARRAY_BUFFER, 0,
		NUM_INSTANCES * sizeof(ovrMatrix4f), GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT));
	for (int i = 0; i < NUM_INSTANCES; i++)
	{
		const ovrMatrix4f rotation = ovrMatrix4f_CreateRotation(
			scene->CubeRotations[i].x * simulation->CurrentRotation.x,
			scene->CubeRotations[i].y * simulation->CurrentRotation.y,
			scene->CubeRotations[i].z * simulation->CurrentRotation.z);
		const ovrMatrix4f translation = ovrMatrix4f_CreateTranslation(
			scene->CubePositions[i].x,
			scene->CubePositions[i].y,
			scene->CubePositions[i].z);
		const ovrMatrix4f transform = ovrMatrix4f_Multiply(&translation, &rotation);
		cubeTransforms[i] = ovrMatrix4f_Transpose(&transform);
	}
	GL(glUnmapBuffer(GL_ARRAY_BUFFER));
	GL(glBindBuffer(GL_ARRAY_BUFFER, 0));

	// Calculate the center view matrix.
	const ovrHeadModelParms headModelParms = vrapi_DefaultHeadModelParms();

	// Render the eye images.
	for (int eye = 0; eye < VRAPI_FRAME_LAYER_EYE_MAX; eye++)
	{
		// updated sensor prediction for each eye (updates orientation, not position)
#if REDUCED_LATENCY
		ovrTracking updatedTracking = vrapi_GetPredictedTracking(ovr, tracking->HeadPose.TimeInSeconds);
		updatedTracking.HeadPose.Pose.Position = tracking->HeadPose.Pose.Position;
#else
		ovrTracking updatedTracking = *tracking;
#endif

		// Calculate the center view matrix.
		const ovrMatrix4f centerEyeViewMatrix = vrapi_GetCenterEyeViewMatrix(&headModelParms, &updatedTracking, NULL);
		const ovrMatrix4f eyeViewMatrix = vrapi_GetEyeViewMatrix(&headModelParms, &centerEyeViewMatrix, eye);

		ovrFramebuffer * frameBuffer = &renderer->FrameBuffer[eye];
		ovrFramebuffer_SetCurrent(frameBuffer);

		GL(glEnable(GL_SCISSOR_TEST));
		GL(glDepthMask(GL_TRUE));
		GL(glEnable(GL_DEPTH_TEST));
		GL(glDepthFunc(GL_LEQUAL));
		GL(glViewport(0, 0, frameBuffer->Width, frameBuffer->Height));
		GL(glScissor(0, 0, frameBuffer->Width, frameBuffer->Height));
		GL(glClearColor(0.125f, 0.0f, 0.125f, 1.0f));
		GL(glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
		GL(glUseProgram(scene->Program.Program));
		GL(glUniformMatrix4fv(scene->Program.Uniforms[UNIFORM_VIEW_MATRIX], 1, GL_TRUE, (const GLfloat *)eyeViewMatrix.M[0]));
		GL(glUniformMatrix4fv(scene->Program.Uniforms[UNIFORM_PROJECTION_MATRIX], 1, GL_TRUE, (const GLfloat *)renderer->ProjectionMatrix.M[0]));
		GL(glBindVertexArray(scene->Cube.VertexArrayObject));
		GL(glDrawElementsInstanced(GL_TRIANGLES, scene->Cube.IndexCount, GL_UNSIGNED_SHORT, NULL, NUM_INSTANCES));
		GL(glBindVertexArray(0));
		GL(glUseProgram(0));

		// Explicitly clear the border texels to black because OpenGL-ES does not support GL_CLAMP_TO_BORDER.
		{
			// Clear to fully opaque black.
			GL(glClearColor(0.0f, 0.0f, 0.0f, 1.0f));
			// bottom
			GL(glScissor(0, 0, frameBuffer->Width, 1));
			GL(glClear(GL_COLOR_BUFFER_BIT));
			// top
			GL(glScissor(0, frameBuffer->Height - 1, frameBuffer->Width, 1));
			GL(glClear(GL_COLOR_BUFFER_BIT));
			// left
			GL(glScissor(0, 0, 1, frameBuffer->Height));
			GL(glClear(GL_COLOR_BUFFER_BIT));
			// right
			GL(glScissor(frameBuffer->Width - 1, 0, 1, frameBuffer->Height));
			GL(glClear(GL_COLOR_BUFFER_BIT));
		}

		ovrFramebuffer_Resolve(frameBuffer);

		parms.Layers[VRAPI_FRAME_LAYER_TYPE_WORLD].Textures[eye].ColorTextureSwapChain = frameBuffer->ColorTextureSwapChain;
		parms.Layers[VRAPI_FRAME_LAYER_TYPE_WORLD].Textures[eye].TextureSwapChainIndex = frameBuffer->TextureSwapChainIndex;
		parms.Layers[VRAPI_FRAME_LAYER_TYPE_WORLD].Textures[eye].TexCoordsFromTanAngles = renderer->TexCoordsTanAnglesMatrix;
		parms.Layers[VRAPI_FRAME_LAYER_TYPE_WORLD].Textures[eye].HeadPose = updatedTracking.HeadPose;

		ovrFramebuffer_Advance(frameBuffer);
	}

	ovrFramebuffer_SetNone();

	return parms;
}

//================================================================================
//
// ovrApp
//
//================================================================================

typedef enum
{
	BACK_BUTTON_STATE_NONE,
	BACK_BUTTON_STATE_PENDING_DOUBLE_TAP,
	BACK_BUTTON_STATE_PENDING_SHORT_PRESS,
	BACK_BUTTON_STATE_SKIP_UP
} ovrBackButtonState;

typedef struct
{
	ovrJava				Java;
	ovrEgl				Egl;
	ANativeWindow *		NativeWindow;
	bool				Resumed;
	ovrMobile *			Ovr;
	ovrScene			Scene;
	ovrSimulation		Simulation;
	long long			FrameIndex;
	int					MinimumVsyncs;
	ovrBackButtonState	BackButtonState;
	bool				BackButtonDown;
	double				BackButtonDownStartTime;
#if MULTI_THREADED
	ovrRenderThread		RenderThread;
#else
	ovrRenderer			Renderer;
#endif
} ovrApp;

static void ovrApp_Clear(ovrApp * app)
{
	app->Java.Vm = NULL;
	app->Java.Env = NULL;
	app->Java.ActivityObject = NULL;
	app->NativeWindow = NULL;
	app->Resumed = false;
	app->Ovr = NULL;
	app->FrameIndex = 1;
	app->MinimumVsyncs = 1;
	app->BackButtonState = BACK_BUTTON_STATE_NONE;
	app->BackButtonDown = false;
	app->BackButtonDownStartTime = 0.0;

	ovrEgl_Clear(&app->Egl);
	ovrScene_Clear(&app->Scene);
	ovrSimulation_Clear(&app->Simulation);
#if MULTI_THREADED
	ovrRenderThread_Clear(&app->RenderThread);
#else
	ovrRenderer_Clear(&app->Renderer);
#endif
}

static void ovrApp_PushBlackFinal(ovrApp * app, const ovrPerformanceParms * perfParms)
{
#if MULTI_THREADED
	ovrRenderThread_Submit(&app->RenderThread, app->Ovr,
		RENDER_BLACK_FINAL, app->FrameIndex, app->MinimumVsyncs, perfParms,
		NULL, NULL, NULL);
#else
	ovrFrameParms frameParms = vrapi_DefaultFrameParms(&app->Java, VRAPI_FRAME_INIT_BLACK_FINAL, NULL);
	frameParms.FrameIndex = app->FrameIndex;
	frameParms.PerformanceParms = *perfParms;
	vrapi_SubmitFrame(app->Ovr, &frameParms);
#endif
}

/*
struct engine {
    struct android_app* app;

    ASensorManager* sensorManager;
    const ASensor* accelerometerSensor;
    ASensorEventQueue* sensorEventQueue;

    int animating;
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    int32_t width;
    int32_t height;
};

static int engine_init_display(struct engine* engine) {
    // initialize OpenGL ES and EGL

    const EGLint attribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_BLUE_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_RED_SIZE, 8,
            EGL_NONE
    };
    EGLint w, h, dummy, format;
    EGLint numConfigs;
    EGLConfig config;
    EGLSurface surface;
    EGLContext context;

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);

    eglInitialize(display, 0, 0);

    eglChooseConfig(display, attribs, &config, 1, &numConfigs);

    eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &format);

    ANativeWindow_setBuffersGeometry(engine->app->window, 0, 0, format);

    surface = eglCreateWindowSurface(display, config, engine->app->window, NULL);
    context = eglCreateContext(display, config, NULL, NULL);

    if (eglMakeCurrent(display, surface, surface, context) == EGL_FALSE) {
        LOGW("Unable to eglMakeCurrent");
        return -1;
    }

    eglQuerySurface(display, surface, EGL_WIDTH, &w);
    eglQuerySurface(display, surface, EGL_HEIGHT, &h);

    engine->display = display;
    engine->context = context;
    engine->surface = surface;
    engine->width = w;
    engine->height = h;
	engine->animating = 1;

    // Initialize GL state.
    glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);
    glEnable(GL_CULL_FACE);
    //glShadeModel(GL_SMOOTH);
    glDisable(GL_DEPTH_TEST);

    return 0;
}

float r = 0;
float g = 1.0;
float b = 0;

static void engine_draw_frame() {
    if (engine->display == NULL) {
        // No display.
        return;
    }

	r += 0.01f;
	if (r > 1.0f)
	{
		r -= 1.0f;
	}
	b += 0.01f;
	if (b > 1.0f)
	{
		b -= 1.0f;
	}

    // Just fill the screen with a color.
    //glClearColor(((float)engine->state.x)/engine->width, engine->state.angle, ((float)engine->state.y)/engine->height, 1);
	glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    eglSwapBuffers(engine->display, engine->surface);
}

static void engine_term_display(struct engine* engine) {
    if (engine->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(engine->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (engine->context != EGL_NO_CONTEXT) {
            eglDestroyContext(engine->display, engine->context);
        }
        if (engine->surface != EGL_NO_SURFACE) {
            eglDestroySurface(engine->display, engine->surface);
        }
        eglTerminate(engine->display);
    }
    engine->animating = 0;
    engine->display = EGL_NO_DISPLAY;
    engine->context = EGL_NO_CONTEXT;
    engine->surface = EGL_NO_SURFACE;
}
*/

static void ovrApp_HandleVrModeChanges(ovrApp * app)
{
	if (app->NativeWindow != NULL && app->Egl.MainSurface == EGL_NO_SURFACE)
	{
		ovrEgl_CreateSurface(&app->Egl, app->NativeWindow);
	}

	if (app->Resumed != false && app->NativeWindow != NULL)
	{
		if (app->Ovr == NULL)
		{
			ovrModeParms parms = vrapi_DefaultModeParms(&app->Java);
			parms.ResetWindowFullscreen = false;	// No need to reset the FLAG_FULLSCREEN window flag when using a View

			LOGI("        eglGetCurrentSurface( EGL_DRAW ) = %p", eglGetCurrentSurface(EGL_DRAW));

			app->Ovr = vrapi_EnterVrMode(&parms);

			LOGI("        vrapi_EnterVrMode()");
			LOGI("        eglGetCurrentSurface( EGL_DRAW ) = %p", eglGetCurrentSurface(EGL_DRAW));
		}
	}
	else
	{
		if (app->Ovr != NULL)
		{
#if MULTI_THREADED
			// Make sure the renderer thread is no longer using the ovrMobile.
			ovrRenderThread_Wait(&app->RenderThread);
#endif
			LOGI("        eglGetCurrentSurface( EGL_DRAW ) = %p", eglGetCurrentSurface(EGL_DRAW));

			vrapi_LeaveVrMode(app->Ovr);
			app->Ovr = NULL;

			LOGI("        vrapi_LeaveVrMode()");
			LOGI("        eglGetCurrentSurface( EGL_DRAW ) = %p", eglGetCurrentSurface(EGL_DRAW));
		}
	}

	if (app->NativeWindow == NULL && app->Egl.MainSurface != EGL_NO_SURFACE)
	{
		ovrEgl_DestroySurface(&app->Egl);
	}
}

static void ovrApp_BackButtonAction(ovrApp * app, const ovrPerformanceParms * perfParms)
{
	if (app->BackButtonState == BACK_BUTTON_STATE_PENDING_DOUBLE_TAP)
	{
		LOGI("back button double tap");
		app->BackButtonState = BACK_BUTTON_STATE_SKIP_UP;
	}
	else if (app->BackButtonState == BACK_BUTTON_STATE_PENDING_SHORT_PRESS && !app->BackButtonDown)
	{
		if ((vrapi_GetTimeInSeconds() - app->BackButtonDownStartTime) > BACK_BUTTON_DOUBLE_TAP_TIME_IN_SECONDS)
		{
			LOGI("back button short press");
			LOGI("        ovrApp_PushBlackFinal()");
			ovrApp_PushBlackFinal(app, perfParms);
			LOGI("        ovr_StartSystemActivity( %s )", PUI_CONFIRM_QUIT);
			ovr_StartSystemActivity(&app->Java, PUI_CONFIRM_QUIT, NULL);
			app->BackButtonState = BACK_BUTTON_STATE_NONE;
		}
	}
	else if (app->BackButtonState == BACK_BUTTON_STATE_NONE && app->BackButtonDown)
	{
		if ((vrapi_GetTimeInSeconds() - app->BackButtonDownStartTime) > BACK_BUTTON_LONG_PRESS_TIME_IN_SECONDS)
		{
			LOGI("back button long press");
			LOGI("        ovrApp_PushBlackFinal()");
			ovrApp_PushBlackFinal(app, perfParms);
			LOGI("        ovr_StartSystemActivity( %s )", PUI_GLOBAL_MENU);
			ovr_StartSystemActivity(&app->Java, PUI_GLOBAL_MENU, NULL);
			app->BackButtonState = BACK_BUTTON_STATE_SKIP_UP;
		}
	}
}

static int ovrApp_HandleKeyEvent(ovrApp * app, const int keyCode, const int action)
{
	// Handle GearVR back button.
	if (keyCode == AKEYCODE_BACK)
	{
		if (action == AKEY_EVENT_ACTION_DOWN)
		{
			if (!app->BackButtonDown)
			{
				if ((vrapi_GetTimeInSeconds() - app->BackButtonDownStartTime) < BACK_BUTTON_DOUBLE_TAP_TIME_IN_SECONDS)
				{
					app->BackButtonState = BACK_BUTTON_STATE_PENDING_DOUBLE_TAP;
				}
				app->BackButtonDownStartTime = vrapi_GetTimeInSeconds();
			}
			app->BackButtonDown = true;
		}
		else if (action == AKEY_EVENT_ACTION_UP)
		{
			if (app->BackButtonState == BACK_BUTTON_STATE_NONE)
			{
				if ((vrapi_GetTimeInSeconds() - app->BackButtonDownStartTime) < BACK_BUTTON_SHORT_PRESS_TIME_IN_SECONDS)
				{
					app->BackButtonState = BACK_BUTTON_STATE_PENDING_SHORT_PRESS;
				}
			}
			else if (app->BackButtonState == BACK_BUTTON_STATE_SKIP_UP)
			{
				app->BackButtonState = BACK_BUTTON_STATE_NONE;
			}
			app->BackButtonDown = false;
		}
		return 1;
	}
	return 0;
}

static int ovrApp_HandleTouchEvent(ovrApp * app, const int action, const float x, const float y)
{
	// Handle GearVR touch pad.
	if (app->Ovr != NULL && action == AMOTION_EVENT_ACTION_UP)
	{
#if 0
		// Cycle through 60Hz, 30Hz, 20Hz and 15Hz synthesis.
		app->MinimumVsyncs++;
		if (app->MinimumVsyncs > 4)
		{
			app->MinimumVsyncs = 1;
		}
		LOGI("        MinimumVsyncs = %d", app->MinimumVsyncs);
#endif
	}
	return 1;
}

static void ovrApp_HandleSystemEvents(ovrApp * app)
{
	// Handle any pending system activity events.
	size_t const MAX_EVENT_SIZE = 4096;
	char eventBuffer[MAX_EVENT_SIZE];

	for (eVrApiEventStatus status = ovr_GetNextPendingEvent(eventBuffer, MAX_EVENT_SIZE);
		status >= VRAPI_EVENT_PENDING;
		status = ovr_GetNextPendingEvent(eventBuffer, MAX_EVENT_SIZE))
	{
		if (status != VRAPI_EVENT_PENDING)
		{
			if (status != VRAPI_EVENT_CONSUMED)
			{
				LOGE("Error %i handing System Activities Event", status);
			}
			continue;
		}

		// parse JSON and handle event
	}
}

//================================================================================
//
// Native Activity
//
//================================================================================

static int32_t app_handle_input(struct android_app* app, AInputEvent* event)
{
	ovrApp * appState = (ovrApp *)app->userData;
	const int type = AInputEvent_getType(event);
	if (type == AINPUT_EVENT_TYPE_KEY)
	{
		const int keyCode = AKeyEvent_getKeyCode(event);
		const int action = AKeyEvent_getAction(event);
		return ovrApp_HandleKeyEvent(appState, keyCode, action);
	}
	else if (type == AINPUT_EVENT_TYPE_MOTION)
	{
		const int source = AInputEvent_getSource(event);
		// Events with source == AINPUT_SOURCE_TOUCHSCREEN come from the phone's builtin touch screen.
		// Events with source == AINPUT_SOURCE_MOUSE come from the trackpad on the right side of the GearVR.
		if (source == AINPUT_SOURCE_TOUCHSCREEN || source == AINPUT_SOURCE_MOUSE)
		{
			const int action = AKeyEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
			const float x = AMotionEvent_getRawX(event, 0);
			const float y = AMotionEvent_getRawY(event, 0);
			return ovrApp_HandleTouchEvent(appState, action, x, y);
		}
	}
	return 0;

    //struct engine* engine = (struct engine*)app->userData;
	/*const int type = AInputEvent_getType(event);
	const int source = AInputEvent_getSource(event);
	LOGI("type = %d - source = %x (%d)", type, source, source);
	if (type == AINPUT_EVENT_TYPE_MOTION)
	{
		if (source == AINPUT_SOURCE_JOYSTICK)
		{
			float lx = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0);
			float ly = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0);
			float rx = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RX, 0);
			float ry = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RY, 0);
			float rt = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RTRIGGER, 0);
			float lt = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_LTRIGGER, 0);

			LOGI("[2] - MOTION: lx=%f, ly=%f, rx=%f, ry=%f, rt=%f, lf=%f", lx, ly, rx, ry, rt, lt);
		}
		float rawX = AMotionEvent_getRawX(event, 0);
		float rawY = AMotionEvent_getRawY(event, 0);
		LOGI("[1] - MOTION: x = %f, y = %f", rawX, rawY);
        return 1;
    }
	else if (type == AINPUT_EVENT_TYPE_KEY)
	{
		LOGI("KEY: keycode = %d", AKeyEvent_getKeyCode(event));
		//if (AKeyEvent_getKeyCode(event) == AKEYCODE_BACK)
		//{
			//FATCAT_LOG("BACK KEY PRESSED!!!!!!");
			//return 1; // <-- prevent default handler
		//};
		return 1;
	}
    return 0;*/
}

static void app_handle_cmd(struct android_app* app, int32_t cmd)
{
	ovrApp * appState = (ovrApp *)app->userData;

	switch (cmd)
	{
		// There is no APP_CMD_CREATE. The ANativeActivity creates the
		// application thread from onCreate(). The application thread
		// then calls android_main().
		case APP_CMD_START:
		{
			LOGI("onStart()");
			LOGI("    APP_CMD_START");
			break;
		}
		case APP_CMD_RESUME:
		{
			LOGI("onResume()");
			LOGI("    APP_CMD_RESUME");
			appState->Resumed = true;
			break;
		}
		case APP_CMD_PAUSE:
		{
			LOGI("onPause()");
			LOGI("    APP_CMD_PAUSE");
			appState->Resumed = false;
			break;
		}
		case APP_CMD_STOP:
		{
			LOGI("onStop()");
			LOGI("    APP_CMD_STOP");
			break;
		}
		case APP_CMD_DESTROY:
		{
			LOGI("onDestroy()");
			LOGI("    APP_CMD_DESTROY");
			appState->NativeWindow = NULL;
			break;
		}
		case APP_CMD_INIT_WINDOW:
		{
			LOGI("surfaceCreated()");
			LOGI("    APP_CMD_INIT_WINDOW");
			appState->NativeWindow = app->window;
			break;
		}
		case APP_CMD_TERM_WINDOW:
		{
			LOGI("surfaceDestroyed()");
			LOGI("    APP_CMD_TERM_WINDOW");
			appState->NativeWindow = NULL;
			break;
		}
    }

    /*struct engine* engine = (struct engine*)app->userData;
    switch (cmd) {
        case APP_CMD_SAVE_STATE:
            // The system has asked us to save our current state.  Do so.
            break;
        case APP_CMD_INIT_WINDOW:
            // The window is being shown, get it ready.
            if (engine->app->window != NULL) {
                engine_init_display(engine);
                engine_draw_frame(engine);
            }
            break;
        case APP_CMD_TERM_WINDOW:
            // The window is being hidden or closed, clean it up.
            engine_term_display(engine);
            break;
        case APP_CMD_GAINED_FOCUS:
            // When our app gains focus, we start monitoring the accelerometer.
            if (engine->accelerometerSensor != NULL) {
                ASensorEventQueue_enableSensor(engine->sensorEventQueue,
                        engine->accelerometerSensor);
                // We'd like to get 60 events per second (in us).
                ASensorEventQueue_setEventRate(engine->sensorEventQueue,
                        engine->accelerometerSensor, (1000L/60)*1000);
            }
            break;
        case APP_CMD_LOST_FOCUS:
            // When our app loses focus, we stop monitoring the accelerometer.
            // This is to avoid consuming battery while not being used.
            if (engine->accelerometerSensor != NULL) {
                ASensorEventQueue_disableSensor(engine->sensorEventQueue,
                        engine->accelerometerSensor);
            }
            // Also stop animating.
            engine->animating = 0;
            engine_draw_frame(engine);
            break;
    }*/
}

void android_main(struct android_app* app)
{
    // Make sure glue isn't stripped.
    app_dummy();

	LOGI("----------------------------------------------------------------");
	LOGI("android_app_entry()");
	LOGI("    android_main()");

	ovrJava java;
	java.Vm = app->activity->vm;
	java.Vm->AttachCurrentThread(&java.Env, NULL);
	java.ActivityObject = app->activity->clazz;

	const ovrInitParms initParms = vrapi_DefaultInitParms(&java);
	vrapi_Initialize(&initParms);

	ovrApp appState;
	ovrApp_Clear(&appState);
	appState.Java = java;

	ovrEgl_CreateContext(&appState.Egl, NULL);

	ovrPerformanceParms perfParms = vrapi_DefaultPerformanceParms();
	perfParms.CpuLevel = CPU_LEVEL;
	perfParms.GpuLevel = GPU_LEVEL;
	perfParms.MainThreadTid = gettid();

#if MULTI_THREADED
	ovrRenderThread_Create(&appState.RenderThread, &appState.Java, &appState.Egl);
	// Also set the renderer thread to SCHED_FIFO.
	perfParms.RenderThreadTid = ovrRenderThread_GetTid(&appState.RenderThread);
#else
	const ovrHmdInfo hmdInfo = vrapi_GetHmdInfo(&appState.Java);
	ovrRenderer_Create(&appState.Renderer, &hmdInfo);
#endif

	app->userData = &appState;
	app->onAppCmd = app_handle_cmd;
	app->onInputEvent = app_handle_input;

	while (app->destroyRequested == 0)
	{
		for (;;)
		{
			int events;
			struct android_poll_source * source;
			const int timeoutMilliseconds = (appState.Ovr == NULL && app->destroyRequested == 0) ? -1 : 0;
			if (ALooper_pollAll(timeoutMilliseconds, NULL, &events, (void **)&source) < 0)
			{
				break;
			}

			// Process this event.
			if (source != NULL)
			{
				source->process(app, source);
			}

			ovrApp_HandleVrModeChanges(&appState);
		}

		ovrApp_BackButtonAction(&appState, &perfParms);
		ovrApp_HandleSystemEvents(&appState);

		if (appState.Ovr == NULL)
		{
			continue;
		}

		if (!ovrScene_IsCreated(&appState.Scene))
		{
#if MULTI_THREADED
			// Show a loading icon.
			ovrRenderThread_Submit(&appState.RenderThread, appState.Ovr,
				RENDER_LOADING_ICON, appState.FrameIndex, appState.MinimumVsyncs, &perfParms,
				NULL, NULL, NULL);
#else
			// Show a loading icon.
			ovrFrameParms frameParms = vrapi_DefaultFrameParms(&appState.Java, VRAPI_FRAME_INIT_LOADING_ICON_FLUSH, NULL);
			frameParms.FrameIndex = appState.FrameIndex;
			frameParms.PerformanceParms = perfParms;
			vrapi_SubmitFrame(appState.Ovr, &frameParms);
#endif

			// Create the scene.
			ovrScene_Create(&appState.Scene);
		}
		
		// This is the only place the frame index is incremented, right before
		// calling vrapi_GetPredictedDisplayTime().
		appState.FrameIndex++;

		// Get the HMD pose, predicted for the middle of the time period during which
		// the new eye images will be displayed. The number of frames predicted ahead
		// depends on the pipeline depth of the engine and the synthesis rate.
		// The better the prediction, the less black will be pulled in at the edges.
		const double predictedDisplayTime = vrapi_GetPredictedDisplayTime(appState.Ovr, appState.FrameIndex);
		const ovrTracking baseTracking = vrapi_GetPredictedTracking(appState.Ovr, predictedDisplayTime);

		// Apply the head-on-a-stick model if there is no positional tracking.
		const ovrHeadModelParms headModelParms = vrapi_DefaultHeadModelParms();
		const ovrTracking tracking = vrapi_ApplyHeadModel(&headModelParms, &baseTracking);

		// Advance the simulation based on the predicted display time.
		ovrSimulation_Advance(&appState.Simulation, predictedDisplayTime);

#if MULTI_THREADED
		// Render the eye images on a separate thread.
		ovrRenderThread_Submit( &appState.RenderThread, appState.Ovr,
			RENDER_FRAME, appState.FrameIndex, appState.MinimumVsyncs, &perfParms,
			&appState.Scene, &appState.Simulation, &tracking );
#else
		// Render eye images and setup ovrFrameParms using ovrTracking.
		const ovrFrameParms frameParms = ovrRenderer_RenderFrame(&appState.Renderer, &appState.Java,
			appState.FrameIndex, appState.MinimumVsyncs, &perfParms,
			&appState.Scene, &appState.Simulation, &tracking,
			appState.Ovr);

		// Hand over the eye images to the time warp.
		vrapi_SubmitFrame(appState.Ovr, &frameParms);
#endif
    }

#if MULTI_THREADED
	ovrRenderThread_Destroy(&appState.RenderThread);
#else
	ovrRenderer_Destroy(&appState.Renderer);
#endif

	ovrScene_Destroy(&appState.Scene);
	ovrEgl_DestroyContext(&appState.Egl);
	vrapi_Shutdown();

	java.Vm->DetachCurrentThread();
}
//END_INCLUDE(all)
