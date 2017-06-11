#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/g2d_driver.h>

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <assert.h>
#include <unistd.h>
#include <math.h>
#include <sys/time.h>
#include <signal.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include "fbdev_window.h"

#include <sys/ipc.h>
#include <sys/shm.h>
#include <ump/ump.h>
#include <ump/ump_ref_drv.h>
#include <mpv/client.h>

typedef struct{
	ump_handle handle;
	void *addr;
	unsigned int width;
	unsigned int height;
	unsigned int bpp;
	unsigned int size;
}UmpBuf;

/*  share memory  */
typedef struct {
    int frameFlag;
    void *umpBufAddr;
    int umpBufWidth;
    int umpBufHeight;
    int umpBufBpp;
    
    int dataFormat;
    int videoWidth;
    int videoHeight;
}ShmBuf;

typedef struct {
    UmpBuf umpBuf;
    ShmBuf *shmBuf;
    
    GLuint	hVertexLoc;
	GLuint	hVertexTexLoc;
	
	struct fbdev_window native_window;
	EGLDisplay egl_display;
	EGLSurface egl_surface;
	EGLContext context;
	GLuint	program;
	
	struct fbdev_pixmap pixmap;
	EGLImageKHR eglImage;
	GLuint texture; 
}PlayCtx;

#define WINDOW_WIDTH 1920
#define WINDOW_HEIGHT 1080

#define TEXTURE_WIDTH 1280
#define TEXTURE_HEIGHT 720
#define TEXTURE_BPP 32 //only support 16 and 32

#define key_mem 			5555558
int createShareMemory(PlayCtx *playCtx)
{
	int shmid = shmget(key_mem, sizeof(ShmBuf), IPC_CREAT | 0666);
    if(shmid == -1){
        printf("Error: allocate shared memory failed!\n");
        return -1;
    }
    playCtx->shmBuf = (ShmBuf *)shmat(shmid, 0, 0);
    playCtx->shmBuf->frameFlag = 0;
    return 0;
}

//---------------------------------------------------------------------
static const char *vertex_shader_source =
"attribute vec3 g_vPosition;				\n"
"attribute vec2 g_vTexCoord;				\n"
"								\n"
"varying   vec2 g_vVSTexCoord;				\n"
"								\n"
"void main()						\n"
"{								\n"
"    gl_Position  = vec4(g_vPosition, 1.0);	\n"
"    g_vVSTexCoord = g_vTexCoord;				\n"
"}								\n";
static const char *fragment_shader_source =
"   precision mediump float;				\n"
"								\n"
"uniform sampler2D s_texture;				\n"
"varying   vec2 g_vVSTexCoord;				\n"
"								\n"
"void main()						\n"
"{								\n"
"    gl_FragColor = texture2D(s_texture,g_vVSTexCoord);	\n"
"}								\n";
//-----------------------------------------------------------------------

static EGLint const config_attribute_list[] = {
	EGL_RED_SIZE, 8,
	EGL_GREEN_SIZE, 8,
	EGL_BLUE_SIZE, 8,
	EGL_ALPHA_SIZE, 8,
	EGL_BUFFER_SIZE, 32,
	EGL_STENCIL_SIZE, 8,
	EGL_DEPTH_SIZE, 24,
	EGL_SAMPLES, 4,

	EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
	EGL_SURFACE_TYPE, EGL_WINDOW_BIT,// | EGL_PIXMAP_BIT

	EGL_NONE
};

static EGLint window_attribute_list[] = {
	EGL_NONE,
};

static const EGLint context_attribute_list[] = {
	EGL_CONTEXT_CLIENT_VERSION, 2,
	EGL_NONE
};

float VertexPositions[]={
	-1.0f, -1.0f, 0.0f,
	-1.0f, 1.0f, 0.0f,
	1.0f, -1.0f, 0.0f,
	1.0f, 1.0f, 0.0f,
};
float VertexTexCoords[] = {
	0.0f,0.0f,
	0.0f,1.0f,
	1.0f,0.0f,
	1.0f,1.0f,
};
//-------------------------------------------
long long usec = 0;
int fps = 0;
void computeFps(int printOnceNSec)
{
	fps++;
	struct timeval tv;
	struct timezone tz;
	gettimeofday(&tv, &tz);
	if(usec != 0){
		if(tv.tv_sec * 1000000 + tv.tv_usec - usec >= printOnceNSec * 1000000){
			printf("fps = %d\n", fps / printOnceNSec);
			usec = tv.tv_sec * 1000000 + tv.tv_usec;
			fps = 0;
		}
	}
	else{
		usec = tv.tv_sec * 1000000 + tv.tv_usec;
	}
}
//--------------------------------------------------
void g2d_fill(UmpBuf *ump, uint32_t color)
{
	int fd_g2d = open("/dev/g2d", O_RDWR);
	if (fd_g2d <= 0) {
		printf("Cannot open device: /dev/g2d!\n");
		return;
	}
	g2d_fillrect args;
	args.flag = G2D_FIL_NONE;//G2D_FIL_PIXEL_ALPHA;//
	args.dst_image.addr[0] = ump_get_phys_address(ump->handle);
	args.dst_image.w = ump->width;
	args.dst_image.h = ump->height;
	args.dst_image.format = G2D_FMT_RGB565;//G2D_FMT_XRGB8888;//G2D_FMT_ARGB_AYUV8888;//G2D_FMT_BGR565;
	args.dst_image.pixel_seq = G2D_SEQ_NORMAL;//G2D_SEQ_P10;//
	args.dst_rect.x = 0;
	args.dst_rect.y = 0;
	args.dst_rect.w = ump->width;
	args.dst_rect.h = ump->height;
	args.color = color & 0xffffff ;
	args.alpha = color >> 24;
	ioctl(fd_g2d, G2D_CMD_FILLRECT, &args);
	
	close(fd_g2d);
}
void g2d_blit(UmpBuf *srcUmp, UmpBuf *dstUmp)
{
	int fd_g2d = open("/dev/g2d", O_RDWR);
	if (fd_g2d <= 0) {
		printf("Cannot open device: /dev/g2d!\n");
		return;
	}
	g2d_stretchblt args;
	//g2d_blt args;
	args.flag = G2D_BLT_FLIP_VERTICAL | G2D_BLT_NONE;//G2D_BLT_PIXEL_ALPHA;//
	args.src_image.addr[0] = ump_get_phys_address(srcUmp->handle);//shaMemBuf->videoDataAddr0
	//args.src_image.addr[1] = shaMemBuf->videoDataAddr1;
	args.src_image.w = srcUmp->width;
	args.src_image.h = srcUmp->height;
	args.src_image.format = G2D_FMT_RGB565;//G2D_FMT_IYUV422;//G2D_FMT_PYUV411UVC;//;//G2D_FMT_PYUV422UVC//G2D_FMT_ARGB_AYUV8888;//
	args.src_image.pixel_seq = G2D_SEQ_NORMAL;//G2D_SEQ_VUVU;//G2D_SEQ_YVYU;//G2D_SEQ_VYUY;//;//;
	args.src_rect.x = 0;
	args.src_rect.y = 0;
	args.src_rect.w = srcUmp->width;
	args.src_rect.h = srcUmp->height;
	args.dst_image.addr[0] = ump_get_phys_address(dstUmp->handle);
	args.dst_image.w = dstUmp->width;
	args.dst_image.h = dstUmp->height;
	args.dst_image.format = G2D_FMT_RGB565;//G2D_FMT_XRGB8888;//G2D_FMT_ARGB_AYUV8888;//G2D_FMT_BGR565;
	args.dst_image.pixel_seq = G2D_SEQ_NORMAL;
	args.dst_rect.x = 0;
	args.dst_rect.y = 0;
	args.dst_rect.w = dstUmp->width;
	args.dst_rect.h = dstUmp->height;
	//args.dst_x = 0;
	//args.dst_y = 0;
	args.color = 0;
	args.alpha = 0;
	ioctl(fd_g2d, G2D_CMD_STRETCHBLT, &args);
	//ioctl(fd_g2d, G2D_CMD_BITBLT, &args);//
	
	close(fd_g2d);
} 
//---------------------------------------
GLuint esLoadProgram ( const char *vertShaderSrc, const char *fragShaderSrc );

/* function to load in bitmap as a GL texture */
int LoadGLTextures(PlayCtx *playCtx)
{
	glGenTextures( 1, &(playCtx->texture) );
	glBindTexture( GL_TEXTURE_2D, playCtx->texture );
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, playCtx->eglImage);
	if (glGetError() != GL_NO_ERROR){
		printf("glEGLImageTargetTexture2DOES failed!\n");
		return -1;
	}
	printf("texture loaded and created successfully\n");
	return 0;
}
int UpdateGLTextures(PlayCtx *playCtx)
{
	//static unsigned int color = 0xffff00ff;
	//color += 0x40;

	//memset(playCtx->umpBuf.addr, color, playCtx->umpBuf.size);
	//ump_write(playCtx->umpBuf.handle, 0, g_pixel_buf, playCtx->umpBuf.size);
	
	//g2d_blit(&umpbuf1, &umpbuf);
	//g2d_fill(&(playCtx->umpBuf), color);

	return 0;
}

void redrawWindow(PlayCtx *playCtx)
{
	// Bind the vertex attributes
	glVertexAttribPointer( playCtx->hVertexLoc, 3, GL_FLOAT, 0, 0, VertexPositions );
	glEnableVertexAttribArray( playCtx->hVertexLoc );

	glVertexAttribPointer( playCtx->hVertexTexLoc, 2, GL_FLOAT, 0, 0, VertexTexCoords );
	glEnableVertexAttribArray( playCtx->hVertexTexLoc );

	/* Select Our Texture */
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, playCtx->texture);
	/* Drawing Using Triangle strips, draw triangle strips using 4 vertices */
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	// Cleanup
	//glDisableVertexAttribArray( hVertexLoc );
	//glDisableVertexAttribArray( hVertexTexLoc );
}
void printVideoInfo(ShmBuf *shmBuf)
{
	static int printflag = 1;
	if(printflag == 1){//only print once
		printflag = 0;
		printf("+++++++++++++++++videoWidth = %d\n", shmBuf->videoWidth);
		printf("+++++++++++++++++videoHeight = %d\n", shmBuf->videoHeight);
		printf("+++++++++++++++++videoFormat = %d\n", shmBuf->dataFormat);
	}
}
void Redraw(PlayCtx *playCtx)
{
	if(playCtx->shmBuf->frameFlag != 0){
		printVideoInfo(playCtx->shmBuf);
		computeFps(2);

		//UpdateGLTextures();
		redrawWindow(playCtx);
		glFinish();
		eglSwapBuffers(playCtx->egl_display, playCtx->egl_surface);
		playCtx->shmBuf->frameFlag = 0;
	}
}
/*
 * Usage: g2d-opengl-video-render videoFilePath
 */
int main(int argc, char *argv[])
{
	EGLint egl_major, egl_minor;
	EGLConfig config;
	EGLint num_config;
	GLint ret;

	PlayCtx playCtx;//
	//init
	playCtx.native_window.width = WINDOW_WIDTH;
	playCtx.native_window.height = WINDOW_HEIGHT;
	
	playCtx.egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (playCtx.egl_display == EGL_NO_DISPLAY) {
		printf("Error: No display found!\n");
		return -1;
	}

	if (!eglInitialize(playCtx.egl_display, &egl_major, &egl_minor)) {
		printf("Error: eglInitialise failed!\n");
		return -1;
	}
	printf("EGL Version: \"%s\"\n", eglQueryString(playCtx.egl_display, EGL_VERSION));
	printf("EGL Vendor: \"%s\"\n", eglQueryString(playCtx.egl_display, EGL_VENDOR));
	printf("EGL Extensions: \"%s\"\n", eglQueryString(playCtx.egl_display, EGL_EXTENSIONS));

	eglChooseConfig(playCtx.egl_display, config_attribute_list, &config, 1, &num_config);

	playCtx.context = eglCreateContext(playCtx.egl_display, config, EGL_NO_CONTEXT, context_attribute_list);
	if (playCtx.context == EGL_NO_CONTEXT) {
		printf("Error: eglCreateContext failed: 0x%08X\n", eglGetError());
		return -1;
	}
	playCtx.egl_surface = eglCreateWindowSurface(playCtx.egl_display, config, &(playCtx.native_window), window_attribute_list);
	//if(EGL_FALSE == eglSurfaceAttrib(playCtx.egl_display, playCtx.egl_surface, EGL_SWAP_BEHAVIOR, EGL_BUFFER_PRESERVED)){
    //    printf("Warnning:eglSurfaceAttrib failed!\n");
    //}
    if (playCtx.egl_surface == EGL_NO_SURFACE) {
		printf("Error: eglCreateWindowSurface failed: 0x%08X\n", eglGetError());
		return -1;
	}
	if (!eglMakeCurrent(playCtx.egl_display, playCtx.egl_surface, playCtx.egl_surface, playCtx.context)) {
		printf("Error: eglMakeCurrent() failed: 0x%08X\n", eglGetError());
		return -1;
	}
	printf("GL Vendor: \"%s\"\n", glGetString(GL_VENDOR));
	printf("GL Renderer: \"%s\"\n", glGetString(GL_RENDERER));
	printf("GL Version: \"%s\"\n", glGetString(GL_VERSION));
	printf("GL Extensions: \"%s\"\n", glGetString(GL_EXTENSIONS));
//---------------------------------------------------------------------------------------------
	if(createShareMemory(&playCtx) != 0){
		return -1;
	}
	if (ump_open() != UMP_OK) {
        printf("ump_open() failed !!!\n");
        return -1;
    }

    playCtx.umpBuf.width = TEXTURE_WIDTH;
    playCtx.umpBuf.height = TEXTURE_HEIGHT;
    playCtx.umpBuf.bpp = TEXTURE_BPP;
	playCtx.umpBuf.size = playCtx.umpBuf.width * playCtx.umpBuf.height * (playCtx.umpBuf.bpp / 8);
	playCtx.umpBuf.handle = ump_ref_drv_allocate(playCtx.umpBuf.size, UMP_REF_DRV_CONSTRAINT_PHYSICALLY_LINEAR);//UMP_REF_DRV_CONSTRAINT_USE_CACHE
	if (playCtx.umpBuf.handle == UMP_INVALID_MEMORY_HANDLE) {
        printf("ump_ref_drv_allocate failed!!!\n");
        return -1;
    }
	playCtx.umpBuf.addr = ump_mapped_pointer_get(playCtx.umpBuf.handle);
	//ump_write(umpbuf.handle, 0, g_texture_buf, umpbuf.size);
	memset(playCtx.umpBuf.addr, 0x00, playCtx.umpBuf.size);
	printf("create UMP buf succeed...\n");
	
	memset(&(playCtx.pixmap), 0, sizeof(struct fbdev_pixmap));
	playCtx.pixmap.width = playCtx.umpBuf.width;
	playCtx.pixmap.height = playCtx.umpBuf.height;
	playCtx.pixmap.bytes_per_pixel = (playCtx.umpBuf.bpp / 8);
	playCtx.pixmap.buffer_size = playCtx.umpBuf.bpp;
	playCtx.pixmap.red_size = (playCtx.umpBuf.bpp == 16) ? 5 : 8;
	playCtx.pixmap.green_size = (playCtx.umpBuf.bpp == 16) ? 6 : 8;
	playCtx.pixmap.blue_size = (playCtx.umpBuf.bpp == 16) ? 5 : 8;
	playCtx.pixmap.alpha_size = (playCtx.umpBuf.bpp == 32) ? 8 : 0;
	playCtx.pixmap.flags = FBDEV_PIXMAP_SUPPORTS_UMP;//FBDEV_PIXMAP_DEFAULT
	playCtx.pixmap.data = (short unsigned int*)(playCtx.umpBuf.handle);
	playCtx.pixmap.format = 0;
	playCtx.eglImage = eglCreateImageKHR(playCtx.egl_display, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, (EGLClientBuffer)&(playCtx.pixmap), NULL);
	if ((ret = eglGetError()) != EGL_SUCCESS){
		printf("eglCreateImageKHR failed! errno = %d\n", ret);
		return -1;
	}
	playCtx.shmBuf->umpBufAddr = ump_get_phys_address(playCtx.umpBuf.handle);
	playCtx.shmBuf->umpBufWidth = playCtx.umpBuf.width;
	playCtx.shmBuf->umpBufHeight = playCtx.umpBuf.height;
	playCtx.shmBuf->umpBufBpp = playCtx.umpBuf.bpp;
	printf("eglCreateImageKHR succeed...\n");
//----------------------------------------------------------------------------------
	playCtx.program = esLoadProgram ( vertex_shader_source, fragment_shader_source );
	playCtx.hVertexLoc = glGetAttribLocation(playCtx.program, "g_vPosition");
	playCtx.hVertexTexLoc = glGetAttribLocation(playCtx.program, "g_vColor");
//-----------------------------------------
	//create mpv context
	mpv_handle *ctx = mpv_create();
    if (!ctx) {
        printf("failed creating mpv context\n");
        return -1;
    }
    int status;
    //set options
    if((status = mpv_set_option_string(ctx, "vo", "vdpau")) < 0){
    	printf("mpv API error: %s\n", mpv_error_string(status));
    	return -1;
    }
	if((status = mpv_set_option_string(ctx, "hwdec", "vdpau")) < 0){
    	printf("mpv API error: %s\n", mpv_error_string(status));
    	return -1;
    }
    if((status = mpv_set_option_string(ctx, "hwdec-codecs", "all")) < 0){
    	printf("mpv API error: %s\n", mpv_error_string(status));
    	return -1;
    }
    // Done setting up options.
    if((status = mpv_initialize(ctx)) < 0){
    	printf("mpv API error: %s\n", mpv_error_string(status));
    	return -1;
    }
    if(argv[1] == NULL){
    	printf("Usage: g2d-opengl-video-render videoFilePath");
    	return -1;
    }
    // Play video file.
    const char *cmd[] = {"loadfile", argv[1], NULL};
    if((status = mpv_command(ctx, cmd)) < 0){
    	printf("mpv API error: %s\n", mpv_error_string(status));
    	return -1;
    }
//-----------------------------------------
	if (LoadGLTextures(&playCtx) != 0){
		printf("error loading texture!\n");
		return -1;
	}
	glEnable(GL_TEXTURE_2D);
	glUseProgram( playCtx.program );
	glViewport(0, 0, WINDOW_WIDTH, WINDOW_HEIGHT);
	while (1) {
		usleep(2000);
		Redraw(&playCtx);
	}
	mpv_terminate_destroy(ctx);
	
	return 0;
}

GLuint esLoadShader ( GLenum type, const char *shaderSrc )
{
   GLuint shader;
   GLint compiled = 0;

   // Create the shader object
   shader = glCreateShader ( type );

   if ( shader == 0 )
   {
	  return 0;
   }

   // Load the shader source
   glShaderSource ( shader, 1, &shaderSrc, NULL );

   // Compile the shader
   glCompileShader ( shader );

   // Check the compile status
   glGetShaderiv ( shader, GL_COMPILE_STATUS, &compiled );

   if ( !compiled )
   {
	  GLint infoLen = 0;

	  glGetShaderiv ( shader, GL_INFO_LOG_LENGTH, &infoLen );

	  if ( infoLen > 1 )
	  {
		 char *infoLog = (char *)malloc ( sizeof ( char ) * infoLen );

		 glGetShaderInfoLog ( shader, infoLen, NULL, infoLog );
		 printf ( "Error compiling shader:\n%s\n", infoLog );

		 free ( infoLog );
	  }

	  glDeleteShader ( shader );
	  return 0;
   }
   return shader;

}
GLuint esLoadProgram ( const char *vertShaderSrc, const char *fragShaderSrc )
{
   GLuint vertexShader;
   GLuint fragmentShader;
   GLuint programObject;
   GLint linked;

   // Load the vertex/fragment shaders
   vertexShader = esLoadShader ( GL_VERTEX_SHADER, vertShaderSrc );

   if ( vertexShader == 0 )
   {
	  return 0;
   }

   fragmentShader = esLoadShader ( GL_FRAGMENT_SHADER, fragShaderSrc );

   if ( fragmentShader == 0 )
   {
	  glDeleteShader ( vertexShader );
	  return 0;
   }

   // Create the program object
   programObject = glCreateProgram ( );

   if ( programObject == 0 )
   {
	  return 0;
   }

   glAttachShader ( programObject, vertexShader );
   glAttachShader ( programObject, fragmentShader );

   // Link the program
   glLinkProgram ( programObject );

   // Check the link status
   glGetProgramiv ( programObject, GL_LINK_STATUS, &linked );

   if ( !linked )
   {
	  GLint infoLen = 0;

	  glGetProgramiv ( programObject, GL_INFO_LOG_LENGTH, &infoLen );

	  if ( infoLen > 1 )
	  {
		 char *infoLog = (char *)malloc ( sizeof ( char ) * infoLen );

		 glGetProgramInfoLog ( programObject, infoLen, NULL, infoLog );
		 printf ( "Error linking program:\n%s\n", infoLog );

		 free ( infoLog );
	  }

	  glDeleteProgram ( programObject );
	  return 0;
   }

   // Free up no longer needed shader resources
   glDeleteShader ( vertexShader );
   glDeleteShader ( fragmentShader );

   return programObject;
}
