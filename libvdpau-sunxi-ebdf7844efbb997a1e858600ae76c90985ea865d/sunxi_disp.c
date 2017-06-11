/*
 * Copyright (c) 2013-2015 Jens Kuske <jenskuske@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "kernel-headers/sunxi_disp_ioctl.h"
#include "vdpau_private.h"
#include "sunxi_disp.h"

struct sunxi_disp_private
{
	struct sunxi_disp pub;

	int fd;
	int video_layer;
	int osd_layer;
	__disp_layer_info_t video_info;
	__disp_layer_info_t osd_info;
};

static void sunxi_disp_close(struct sunxi_disp *sunxi_disp);
static int sunxi_disp_set_video_layer(struct sunxi_disp *sunxi_disp, int x, int y, int width, int height, output_surface_ctx_t *surface);
static void sunxi_disp_close_video_layer(struct sunxi_disp *sunxi_disp);
static int sunxi_disp_set_osd_layer(struct sunxi_disp *sunxi_disp, int x, int y, int width, int height, output_surface_ctx_t *surface);
static void sunxi_disp_close_osd_layer(struct sunxi_disp *sunxi_disp);

struct sunxi_disp *sunxi_disp_open(int osd_enabled)
{
	struct sunxi_disp_private *disp = calloc(1, sizeof(*disp));

	disp->fd = open("/dev/disp", O_RDWR);
	if (disp->fd == -1)
		goto err_open;

	int tmp = SUNXI_DISP_VERSION;
	if (ioctl(disp->fd, DISP_CMD_VERSION, &tmp) < 0)
		goto err_version;

	uint32_t args[4] = { 0, DISP_LAYER_WORK_MODE_SCALER, 0, 0 };
	disp->video_layer = ioctl(disp->fd, DISP_CMD_LAYER_REQUEST, args);
	if (disp->video_layer == 0)
		goto err_video_layer;

	args[1] = disp->video_layer;
	ioctl(disp->fd, osd_enabled ? DISP_CMD_LAYER_TOP : DISP_CMD_LAYER_BOTTOM, args);

	if (osd_enabled)
	{
		args[1] = DISP_LAYER_WORK_MODE_NORMAL;
		disp->osd_layer = ioctl(disp->fd, DISP_CMD_LAYER_REQUEST, args);
		if (disp->osd_layer == 0)
			goto err_osd_layer;

		args[1] = disp->osd_layer;
		ioctl(disp->fd, DISP_CMD_LAYER_TOP, args);

		disp->osd_info.pipe = 1;
		disp->osd_info.mode = DISP_LAYER_WORK_MODE_NORMAL;
		disp->osd_info.fb.mode = DISP_MOD_INTERLEAVED;
		disp->osd_info.fb.format = DISP_FORMAT_ARGB8888;
		disp->osd_info.fb.seq = DISP_SEQ_ARGB;
		disp->osd_info.fb.cs_mode = DISP_BT601;
	}
	else
	{
		disp->video_info.pipe = 1;
		disp->video_info.ck_enable = 1;

		__disp_colorkey_t ck;
		ck.ck_max.red = ck.ck_min.red = 0;
		ck.ck_max.green = ck.ck_min.green = 1;
		ck.ck_max.blue = ck.ck_min.blue = 2;
		ck.red_match_rule = 2;
		ck.green_match_rule = 2;
		ck.blue_match_rule = 2;

		args[1] = (unsigned long)(&ck);
		ioctl(disp->fd, DISP_CMD_SET_COLORKEY, args);
	}

	disp->video_info.mode = DISP_LAYER_WORK_MODE_SCALER;
	disp->video_info.fb.cs_mode = DISP_BT601;
	disp->video_info.fb.br_swap = 0;

	disp->pub.close = sunxi_disp_close;
	disp->pub.set_video_layer = sunxi_disp_set_video_layer;
	disp->pub.close_video_layer = sunxi_disp_close_video_layer;
	disp->pub.set_osd_layer = sunxi_disp_set_osd_layer;
	disp->pub.close_osd_layer = sunxi_disp_close_osd_layer;

	return (struct sunxi_disp *)disp;

err_osd_layer:
	args[1] = disp->osd_layer;
	ioctl(disp->fd, DISP_CMD_LAYER_RELEASE, args);
err_video_layer:
err_version:
	close(disp->fd);
err_open:
	free(disp);
	return NULL;
}

static void sunxi_disp_close(struct sunxi_disp *sunxi_disp)
{
	struct sunxi_disp_private *disp = (struct sunxi_disp_private *)sunxi_disp;

	uint32_t args[4] = { 0, disp->video_layer, 0, 0 };
	ioctl(disp->fd, DISP_CMD_LAYER_CLOSE, args);
	ioctl(disp->fd, DISP_CMD_LAYER_RELEASE, args);

	if (disp->osd_layer)
	{
		args[1] = disp->osd_layer;
		ioctl(disp->fd, DISP_CMD_LAYER_CLOSE, args);
		ioctl(disp->fd, DISP_CMD_LAYER_RELEASE, args);
	}

	close(disp->fd);
	free(sunxi_disp);
}
#include <sys/time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include "tiled_yuv.h"
#include "kernel-headers/g2d_driver.h"
/*  共享内存结构体  */
typedef struct {
    int frameFlag;
    void *umpBufAddr;
    int umpBufWidth;
    int umpBufHeight;
    int umpBufBpp;
    
    int dataFormat;
    int videoWidth;
    int videoHeight;
}shm_buf;
shm_buf *mem_buf = NULL;
#define key_mem 			5555558
int fd_g2d = -1;
cedrus_mem_t *videoDataY = NULL;
cedrus_mem_t *videoDataUV = NULL;
static void blitVideoToUmpBuffer(shm_buf *shaMemBuf, output_surface_ctx_t *surface, void *dataY, void *dataUV)
{
	if (fd_g2d <= 0) {
		fd_g2d = open("/dev/g2d", O_RDWR);
		if (fd_g2d <= 0) {
			printf("Cannot open device: /dev/g2d!\n");
			return;
		}
	}
	g2d_stretchblt args;
	args.flag = G2D_BLT_FLIP_VERTICAL | G2D_BLT_NONE;
	args.src_image.addr[0] = dataY;
	args.src_image.addr[1] = dataUV;
	args.src_image.w = surface->vs->width;
	args.src_image.h = surface->vs->height;
	args.src_image.format = G2D_FMT_PYUV420UVC;
	args.src_image.pixel_seq = G2D_SEQ_NORMAL;
	args.src_rect.x = surface->video_src_rect.x0;
	args.src_rect.y = surface->video_src_rect.y0;
	args.src_rect.w = surface->video_src_rect.x1 - surface->video_src_rect.x0;
	args.src_rect.h = surface->video_src_rect.y1 - surface->video_src_rect.y0;
	args.dst_image.addr[0] = shaMemBuf->umpBufAddr;
	args.dst_image.w = shaMemBuf->umpBufWidth;
	args.dst_image.h = shaMemBuf->umpBufHeight;
	args.dst_image.format = (shaMemBuf->umpBufBpp == 32) ? G2D_FMT_XRGB8888 : G2D_FMT_RGB565;
	args.dst_image.pixel_seq = G2D_SEQ_NORMAL;
	args.dst_rect.x = 0;
	args.dst_rect.y = 0;
	args.dst_rect.w = shaMemBuf->umpBufWidth;
	args.dst_rect.h = shaMemBuf->umpBufHeight;
	args.color = 0;
	args.alpha = 0;
	ioctl(fd_g2d, G2D_CMD_STRETCHBLT, &args);
}
static void convertVideoDataToUmpBuffer(shm_buf *shaMemBuf, output_surface_ctx_t *surface, void *dataY, void *dataUV)
{
	if(shaMemBuf->frameFlag == 0){
    	shaMemBuf->frameFlag = 0;
    	//cedrus_mem_flush_cache(videoDataY);
    	//cedrus_mem_flush_cache(videoDataUV);
    	blitVideoToUmpBuffer(shaMemBuf, surface, dataY, dataUV);
    	mem_buf->frameFlag = 1;
    }
}
static int sunxi_disp_set_video_layer(struct sunxi_disp *sunxi_disp, int x, int y, int width, int height, output_surface_ctx_t *surface)
{
	struct sunxi_disp_private *disp = (struct sunxi_disp_private *)sunxi_disp;
	
#if 1
	//allocate memory buffer for video data
    if(videoDataY == NULL){
    	videoDataY = cedrus_mem_alloc(surface->vs->device->cedrus, surface->vs->luma_size);
    	if (videoDataY == NULL){
			return 0;
		}
	}
	if(videoDataUV == NULL){
    	videoDataUV = cedrus_mem_alloc(surface->vs->device->cedrus, surface->vs->chroma_size);
    	if (videoDataUV == NULL){
			return 0;
		}
	}
	//get share memory
	if(mem_buf == NULL){
		int shmid = shmget(key_mem, 0, IPC_CREAT | 0666);
    	if(shmid == -1){
        	return 0;
    	}
    	mem_buf = (shm_buf *)shmat(shmid, 0, 0);
    	if((int)mem_buf == -1){
    		mem_buf = NULL;
    		return 0;
    	}
    }
    //mem_buf->dataFormat = surface->vs->source_format;
    //mem_buf->videoWidth = surface->video_src_rect.x1 - surface->video_src_rect.x0;
    //mem_buf->videoHeight = surface->video_src_rect.y1 - surface->video_src_rect.y0;
    //only support YUV420 format
    if(surface->vs->source_format == VDP_YCBCR_FORMAT_NV12){
    	convertVideoDataToUmpBuffer(mem_buf, surface, cedrus_mem_get_phys_addr(surface->yuv->data), 
    								cedrus_mem_get_phys_addr(surface->yuv->data) + surface->vs->luma_size);
    }
	else if (surface->vs->source_format == VDP_YCBCR_FORMAT_YV12){//convert to NV12
		int i,j;
		uint8_t *dst;
		const uint8_t *srcU, *srcV;
		
		srcU = cedrus_mem_get_pointer(surface->yuv->data) + surface->vs->luma_size;
		srcV = cedrus_mem_get_pointer(surface->yuv->data) + surface->vs->luma_size + surface->vs->chroma_size / 2;
		dst = cedrus_mem_get_pointer(videoDataUV);
		for (i = 0, j = 0; i < surface->vs->chroma_size / 2; i++){
			dst[j++] = srcU[i];
			dst[j++] = srcV[i];
		}
		convertVideoDataToUmpBuffer(mem_buf, surface, cedrus_mem_get_phys_addr(surface->yuv->data), cedrus_mem_get_phys_addr(videoDataUV));
	}
	else if (surface->vs->source_format == INTERNAL_YCBCR_FORMAT){
		//cedrus_mem_flush_cache(surface->vs->yuv->data);
		tiled_to_planar(cedrus_mem_get_pointer(surface->vs->yuv->data), cedrus_mem_get_pointer(videoDataY), 
						surface->vs->width, surface->vs->width, surface->vs->height);
		tiled_to_planar(cedrus_mem_get_pointer(surface->vs->yuv->data) + surface->vs->luma_size, 
						cedrus_mem_get_pointer(videoDataUV), surface->vs->width, surface->vs->width, surface->vs->height / 2);
		
		convertVideoDataToUmpBuffer(mem_buf, surface, cedrus_mem_get_phys_addr(videoDataY), cedrus_mem_get_phys_addr(videoDataUV));
	}
#else
	switch (surface->vs->source_format) {
	case VDP_YCBCR_FORMAT_YUYV:
		disp->video_info.fb.mode = DISP_MOD_INTERLEAVED;
		disp->video_info.fb.format = DISP_FORMAT_YUV422;
		disp->video_info.fb.seq = DISP_SEQ_YUYV;
		break;
	case VDP_YCBCR_FORMAT_UYVY:
		disp->video_info.fb.mode = DISP_MOD_INTERLEAVED;
		disp->video_info.fb.format = DISP_FORMAT_YUV422;
		disp->video_info.fb.seq = DISP_SEQ_UYVY;
		break;
	case VDP_YCBCR_FORMAT_NV12:
		disp->video_info.fb.mode = DISP_MOD_NON_MB_UV_COMBINED;
		disp->video_info.fb.format = DISP_FORMAT_YUV420;
		disp->video_info.fb.seq = DISP_SEQ_UVUV;
		break;
	case VDP_YCBCR_FORMAT_YV12:
		disp->video_info.fb.mode = DISP_MOD_NON_MB_PLANAR;
		disp->video_info.fb.format = DISP_FORMAT_YUV420;
		disp->video_info.fb.seq = DISP_SEQ_UVUV;
		break;
	default:
	case INTERNAL_YCBCR_FORMAT:
		disp->video_info.fb.mode = DISP_MOD_MB_UV_COMBINED;
		disp->video_info.fb.format = DISP_FORMAT_YUV420;
		disp->video_info.fb.seq = DISP_SEQ_UVUV;
		break;
	}
	
	disp->video_info.fb.addr[0] = cedrus_mem_get_phys_addr(surface->yuv->data);
	disp->video_info.fb.addr[1] = cedrus_mem_get_phys_addr(surface->yuv->data) + surface->vs->luma_size;
	disp->video_info.fb.addr[2] = cedrus_mem_get_phys_addr(surface->yuv->data) + surface->vs->luma_size + surface->vs->chroma_size / 2;

	disp->video_info.fb.size.width = surface->vs->width;
	disp->video_info.fb.size.height = surface->vs->height;
	disp->video_info.src_win.x = surface->video_src_rect.x0;
	disp->video_info.src_win.y = surface->video_src_rect.y0;
	disp->video_info.src_win.width = surface->video_src_rect.x1 - surface->video_src_rect.x0;
	disp->video_info.src_win.height = surface->video_src_rect.y1 - surface->video_src_rect.y0;
	disp->video_info.scn_win.x = x + surface->video_dst_rect.x0;
	disp->video_info.scn_win.y = y + surface->video_dst_rect.y0;
	disp->video_info.scn_win.width = surface->video_dst_rect.x1 - surface->video_dst_rect.x0;
	disp->video_info.scn_win.height = surface->video_dst_rect.y1 - surface->video_dst_rect.y0;

	if (disp->video_info.scn_win.y < 0)
	{
		int scn_clip = -(disp->video_info.scn_win.y);
		int src_clip = scn_clip * disp->video_info.src_win.height / disp->video_info.scn_win.height;
		disp->video_info.src_win.y += src_clip;
		disp->video_info.src_win.height -= src_clip;
		disp->video_info.scn_win.y = 0;
		disp->video_info.scn_win.height -= scn_clip;
	}

	uint32_t args[4] = { 0, disp->video_layer, (unsigned long)(&disp->video_info), 0 };
	ioctl(disp->fd, DISP_CMD_LAYER_SET_PARA, args);

	ioctl(disp->fd, DISP_CMD_LAYER_OPEN, args);

	// Note: might be more reliable (but slower and problematic when there
	// are driver issues and the GET functions return wrong values) to query the
	// old values instead of relying on our internal csc_change.
	// Since the driver calculates a matrix out of these values after each
	// set doing this unconditionally is costly.
	if (surface->csc_change)
	{
		ioctl(disp->fd, DISP_CMD_LAYER_ENHANCE_OFF, args);
		args[2] = 0xff * surface->brightness + 0x20;
		ioctl(disp->fd, DISP_CMD_LAYER_SET_BRIGHT, args);
		args[2] = 0x20 * surface->contrast;
		ioctl(disp->fd, DISP_CMD_LAYER_SET_CONTRAST, args);
		args[2] = 0x20 * surface->saturation;
		ioctl(disp->fd, DISP_CMD_LAYER_SET_SATURATION, args);
		// hue scale is randomly chosen, no idea how it maps exactly
		args[2] = (32 / 3.14) * surface->hue + 0x20;
		ioctl(disp->fd, DISP_CMD_LAYER_SET_HUE, args);
		ioctl(disp->fd, DISP_CMD_LAYER_ENHANCE_ON, args);
		surface->csc_change = 0;
	}
#endif
	return 0;
}

static void sunxi_disp_close_video_layer(struct sunxi_disp *sunxi_disp)
{
	struct sunxi_disp_private *disp = (struct sunxi_disp_private *)sunxi_disp;

	uint32_t args[4] = { 0, disp->video_layer, 0, 0 };
	ioctl(disp->fd, DISP_CMD_LAYER_CLOSE, args);
#if 1
	//close g2d device
	if(fd_g2d > 0){	
		close(fd_g2d);
		fd_g2d = -1;
	}
	//detach share memory
	if(mem_buf != NULL){
		shmdt(mem_buf);
		mem_buf = NULL;
	}
#endif
}

static int sunxi_disp_set_osd_layer(struct sunxi_disp *sunxi_disp, int x, int y, int width, int height, output_surface_ctx_t *surface)
{
	struct sunxi_disp_private *disp = (struct sunxi_disp_private *)sunxi_disp;

	switch (surface->rgba.format)
	{
	case VDP_RGBA_FORMAT_R8G8B8A8:
		disp->osd_info.fb.br_swap = 1;
		break;
	case VDP_RGBA_FORMAT_B8G8R8A8:
	default:
		disp->osd_info.fb.br_swap = 0;
		break;
	}

	disp->osd_info.fb.addr[0] = cedrus_mem_get_phys_addr(surface->rgba.data);
	disp->osd_info.fb.size.width = surface->rgba.width;
	disp->osd_info.fb.size.height = surface->rgba.height;
	disp->osd_info.src_win.x = surface->rgba.dirty.x0;
	disp->osd_info.src_win.y = surface->rgba.dirty.y0;
	disp->osd_info.src_win.width = surface->rgba.dirty.x1 - surface->rgba.dirty.x0;
	disp->osd_info.src_win.height = surface->rgba.dirty.y1 - surface->rgba.dirty.y0;
	disp->osd_info.scn_win.x = x + surface->rgba.dirty.x0;
	disp->osd_info.scn_win.y = y + surface->rgba.dirty.y0;
	disp->osd_info.scn_win.width = min_nz(width, surface->rgba.dirty.x1) - surface->rgba.dirty.x0;
	disp->osd_info.scn_win.height = min_nz(height, surface->rgba.dirty.y1) - surface->rgba.dirty.y0;

	uint32_t args[4] = { 0, disp->osd_layer, (unsigned long)(&disp->osd_info), 0 };
	ioctl(disp->fd, DISP_CMD_LAYER_SET_PARA, args);

	ioctl(disp->fd, DISP_CMD_LAYER_OPEN, args);

	return 0;
}

static void sunxi_disp_close_osd_layer(struct sunxi_disp *sunxi_disp)
{
	struct sunxi_disp_private *disp = (struct sunxi_disp_private *)sunxi_disp;

	uint32_t args[4] = { 0, disp->osd_layer, 0, 0 };
	ioctl(disp->fd, DISP_CMD_LAYER_CLOSE, args);
}
