/*
 * Copyright (C) 2011 Kevin Bruckert
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define DEBUG
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/fb.h>
#include <linux/delay.h>

#include <linux/freezer.h>
#include <linux/wait.h>
#include <linux/wakelock.h>
#include <linux/earlysuspend.h>
#include <linux/msm_mdp.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <mach/msm_fb.h>
#include <mach/board.h>
#include <linux/workqueue.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/htc_hdmi.h>

#include "include/fb-hdmi.h"
#include "include/sil902x.h"

#if 1
#define HDMI_DBG(s...) printk("[hdmi/mirror]" s)
#else
#define HDMI_DBG(s...) do {} while (0)
#endif

#define MIN(a,b)    ((a) > (b) ? (b) : (a))
#define MAX(a,b)    ((b) > (a) ? (b) : (a))

struct internal_mirror_settings {
    enum MIRROR_ROTATION rotation;
    enum MIRROR_ROTATION currentRotation;
    enum MIRROR_SCALING scaling;
    int overscanX;
    int overscanY;
    int currentOverscanX;
    int currentOverscanY;
    int audioOverHdmi;
    int mirroringState;
};

struct internal_mirror_settings settings = {
    ROTATE_AUTO, 
    ROTATE_0, 
    SCALING_FIT_TO_SCREEN, 
    60, 60, 
    1, 
    false
};

void hdmi_blit_on_vsync(struct mdp_blit_req* req);
struct hdmi_info* hdmi_get_hdmi_info(void);
struct fb_info* hdmi_get_fb_info(void);
int hdmifb_pause(struct fb_info *fb, unsigned int mode);
int hdmi_startMirroring(int reason);
int hdmi_stopMirroring(int reason);
bool hdmi_IsEnabled(void);
bool hdmi_isCableConnected(void);

int getOrientation(void);

static bool calculateBlitDetails(struct fb_info* src, struct fb_info* dst, struct mdp_blit_req* req)
{
    /* Helpers to dereferencing the fb_var_screeninfo data*/
    struct fb_var_screeninfo* srcinfo = &src->var;
    struct fb_var_screeninfo* dstinfo = &dst->var;

    /* Physical screen dimensions */
    unsigned int srcSizeX, srcSizeY;
    unsigned int dstSizeX, dstSizeY;

    /* dstWidth and dstHeight are used to handle rotated destinations */
    unsigned int dstWidth, dstHeight;

    /* Handler to identify need to flip destination */
    bool flipDst;

    /* This is the non-rotated destination rectangle */
    struct mdp_rect dst_rect;

    /* Preinitialized the request block */
    memset(req, 0, sizeof(struct mdp_blit_req));

    /* Configure the source buffer format */
    req->src.width = srcinfo->xres;
    req->src.height = srcinfo->yres;
    req->src.format = MDP_RGBX_8888;
    req->src.memory_id = 0x20000000;

    /* Configure the extra fields */
    req->alpha = MDP_ALPHA_NOP;
    req->transp_mask = MDP_TRANSP_NOP;
    req->sharpening_strength = 64;

    /* Apply the details about the destination */
    req->dst.width = dstinfo->xres;
    req->dst.height = dstinfo->yres;
    req->dst.format = MDP_RGB_565;
    req->dst.memory_id = 0x20000001;

    /* Retrieve the size, in mm of src */
    srcSizeX = srcinfo->width;
    srcSizeY = srcinfo->height;

    /* Destination has to be queried from edid information and is reported in cm */
    edid_get_screen_size(hdmi_get_hdmi_info(), &dstSizeX, &dstSizeY);
    dstSizeX *= 10;
    dstSizeY *= 10;

    /* Protect from divide-by-zero errors */
    if (srcSizeX == 0 || srcSizeY == 0)
    {
        srcSizeX = srcinfo->xres;
        srcSizeY = srcinfo->yres;
    }
    if (dstSizeX == 0 || dstSizeY == 0)
    {
        dstSizeX = dstinfo->xres;
        dstSizeY = dstinfo->yres;
    }

    /* Handle auto rotation */
    if (settings.rotation == ROTATE_AUTO)
    {
        settings.currentRotation = getOrientation();
    }
    else
    {
        settings.currentRotation = settings.rotation;
    }

    /* If we're doing center scaling, remove overscan support */
    settings.currentOverscanX = settings.overscanX;
    settings.currentOverscanY = settings.overscanY;
    if (settings.scaling == SCALING_CENTER)
    {
        settings.currentOverscanX = 0;
        settings.currentOverscanY = 0;
    }

    /* Set rotation flag, and yes, they're backwards to the blitter */
    switch (settings.currentRotation)
    {
    default:
    case ROTATE_0:
        req->flags = MDP_ROT_NOP;
        dstWidth = dstinfo->xres - settings.currentOverscanX;
        dstHeight = dstinfo->yres - settings.currentOverscanY;
        flipDst = false;
        break;
    case ROTATE_90: 
        dstHeight = dstinfo->xres - settings.currentOverscanX;
        dstWidth = dstinfo->yres - settings.currentOverscanY;
        flipDst = true;
        req->flags = MDP_ROT_270;
        break;
    case ROTATE_180:
        req->flags = MDP_ROT_180;
        dstWidth = dstinfo->xres - settings.currentOverscanX;
        dstHeight = dstinfo->yres - settings.currentOverscanY;
        flipDst = false;
        break;
    case ROTATE_270:
        req->flags = MDP_ROT_90;
        dstHeight = dstinfo->xres - settings.currentOverscanX;
        dstWidth = dstinfo->yres - settings.currentOverscanY;
        flipDst = true;
        break;
    }

    // If we're rotating 90/270, we need to virtually flip the screen size
    if (flipDst)
    {
        int tmp = dstSizeX;
        dstSizeX = dstSizeY;
        dstSizeY = tmp;
    }

    /* By default, the source rectange is always the whole display */
    req->src_rect.x = 0;
    req->src_rect.y = 0;
    req->src_rect.w = srcinfo->xres;
    req->src_rect.h = srcinfo->yres;

    /* Now, let's compute the scaling */
    switch (settings.scaling)
    {
    case SCALING_STRETCH:
        {
            // This is the easiest. We just stretch to fill the screen, minus overscan (we'll shift it x/y at the end)
            dst_rect.x = 0;
            dst_rect.y = 0;
            dst_rect.w = req->dst.width - settings.currentOverscanX;
            dst_rect.h = req->dst.height - settings.currentOverscanY;
            
            // We don't need to flip this, since it's already accounted for
            flipDst = false;
        }
        break;

    case SCALING_CENTER:
        {
            // In this regard, we do a pixel-for-pixel mapping, regardless of screen sizes
            if (srcinfo->xres <= dstWidth)
            {
                dst_rect.x = (dstWidth - srcinfo->xres) >> 1;
                dst_rect.w = srcinfo->xres;
            }
            else
            {
                dst_rect.x = 0;
                dst_rect.w = dstWidth;
                req->src_rect.x = (srcinfo->xres - dstWidth) >> 1;
                req->src_rect.w = dstWidth;
            }
            if (srcinfo->yres <= dstHeight)
            {
                dst_rect.y = (dstHeight - srcinfo->yres) >> 1;
                dst_rect.h = srcinfo->yres;
            }
            else
            {
                dst_rect.y = 0;
                dst_rect.h = dstHeight;
                req->src_rect.y = (srcinfo->yres - dstHeight) >> 1;
                req->src_rect.h = dstHeight;
            }
        }
        break;

    case SCALING_FIT_TO_SCREEN:
        {
            // variables: srcSizeX, srcSizeY, dstSizeX, dstSizeY
            int scaleToX = (srcSizeX * dstSizeY) / srcSizeY;
            int scaleToY = (srcSizeY * dstSizeX) / srcSizeX;

            if (scaleToX <= dstSizeX)
            {
                // We're going to scale in the X dimension
                dst_rect.w = (scaleToX * dstWidth) / dstSizeX;
                dst_rect.h = dstHeight;

                // Now find the "center"
                dst_rect.x = (dstWidth - dst_rect.w) >> 1;
                dst_rect.y = 0;
            }
            else
            {
                // We're going to scale in the X dimension
                dst_rect.w = dstWidth;
                dst_rect.h = (scaleToY * dstHeight) / dstSizeY;

                // Now find the "center"
                dst_rect.x = 0;
                dst_rect.y = (dstHeight - dst_rect.h) >> 1;
            }
        }
        break;

    default:
        HDMI_DBG("Invalid scaling mode.\n");
        return false;
    }

    // Transfer back the dst_rect
    if (flipDst)
    {
        req->dst_rect.x = dst_rect.y;
        req->dst_rect.y = dst_rect.x;
        req->dst_rect.w = dst_rect.h;
        req->dst_rect.h = dst_rect.w;
    }
    else
    {
        req->dst_rect.x = dst_rect.x;
        req->dst_rect.y = dst_rect.y;
        req->dst_rect.w = dst_rect.w;
        req->dst_rect.h = dst_rect.h;
    }
    req->dst_rect.x += (settings.currentOverscanX >> 1);
    req->dst_rect.y += (settings.currentOverscanY >> 1);

    return true;
}

static void dump_mdp_rect(const char* header, struct mdp_rect* rect)
{
    printk("  %s (mdp_rect):\n", header);
    printk("    x: %d\n", rect->x);
    printk("    y: %d\n", rect->y);
    printk("    w: %d\n", rect->w);
    printk("    h: %d\n", rect->h);
    return;
}

static void dump_mdp_img(const char* header, struct mdp_img* img)
{
    printk("  %s (mdp_img):\n", header);
    printk("    width: %d\n", img->width);
    printk("    height: %d\n", img->height);
    printk("    format: %d\n", img->format);
    printk("    offset: %d\n", img->offset);
    printk("    memory_id: %d\n", img->memory_id);
    printk("    priv: %d\n", img->priv);
    return;
}

static void dump_blitter_data(struct mdp_blit_req *req)
{
    printk("mbp_blit_req:\n");
    dump_mdp_img("src", &req->src);
    dump_mdp_img("dst", &req->dst);
    dump_mdp_rect("src_rect", &req->src_rect);
    dump_mdp_rect("dst_rect", &req->dst_rect);
    return;
}

// Structures that are contained in surface flinger block
struct display_cblk_t
{
    uint16_t    w;
    uint16_t    h;
    uint8_t     format;
    uint8_t     orientation;
    uint8_t     reserved[2];
    float       fps;
    float       density;
    float       xdpi;
    float       ydpi;
    uint32_t    pad[2];
};

struct surface_flinger_cblk_t   // 4KB max
{
    uint8_t         connected;
    uint8_t         reserved[3];
    uint32_t        pad[7];
    struct display_cblk_t  displays[4];
};

int read_surfaceflinger_file(void* buffer, int len);

int getOrientation(void)
{
    struct surface_flinger_cblk_t data;
    int orient;
    int ret;

    ret = read_surfaceflinger_file((void*) &data, sizeof(struct surface_flinger_cblk_t));
    if (ret != sizeof(struct surface_flinger_cblk_t))
    {
        HDMI_DBG("read_surfaceflinger_file failed to read enough bytes (returned %d)\n", ret);
        return 1;
    }

    orient = (int) data.displays[0].orientation;
    orient++;
    if (orient < 1 || orient > 4)
    {
        HDMI_DBG("Invalid orientation! %d\n", orient);
        orient = 1;
    }
    return orient;
}

static bool mirroring_needReqUpdate(struct fb_info* src, struct fb_info* dst, struct mdp_blit_req* req)
{
    static struct fb_var_screeninfo lastSrc = { 0 };

    lastSrc.yoffset = src->var.yoffset;
    if (memcmp(&lastSrc, &src->var, sizeof(struct fb_var_screeninfo)) != 0)
    {
        memcpy(&lastSrc, &src->var, sizeof(struct fb_var_screeninfo));
        printk("lastSrc != src->var\n");
        return true;
    }

    // Check for simple width/height matching
    if (req->src.width != src->var.xres || 
        req->src.height != src->var.yres)
    {
        return true;
    }

    if (settings.rotation == ROTATE_AUTO && 
        settings.currentRotation != getOrientation())
    {
        return true;
    }
    return false;
}

void eraseFrameBuffer(struct fb_info* dst)
{
    struct fb_fillrect rect;

    rect.dx = 0;
    rect.dy = 0;
    rect.width = dst->var.xres;
    rect.height = dst->var.yres;
    rect.color = 0;
    rect.rop = 0;

    cfb_fillrect(dst, &rect);
    return;
}

bool mirroring_reportBlit(struct fb_info* src, struct fb_info* dst, struct mdp_blit_req* req)
{
    if (mirroring_needReqUpdate(src, dst, req))
    {
        HDMI_DBG("Recalculating blitter details\n");
        if (!calculateBlitDetails(src, dst, req))
        {
            HDMI_DBG("Failed to recalculate blitter details\n");
            return false;
        }
        dump_blitter_data(req);
        eraseFrameBuffer(dst);
    }

    // Update the yOffset
    req->src.offset = (src->var.yoffset * src->var.xres_virtual * (src->var.bits_per_pixel >> 3));

    hdmi_blit_on_vsync(req);
    return true;
}

static bool mirroring_setResolution(struct hdmi_info *hdmi)
{
    struct fb_var_screeninfo var;
    int xres, yres;

    memset(&var, 0, sizeof(struct fb_var_screeninfo));

    if (!edid_get_best_resolution(hdmi, &xres, &yres))
    {
        return false;
    }

    HDMI_DBG("Setting HDMI to %dx%d\n", xres, yres);
    var.xres = xres;
    var.yres = yres;
    var.xres_virtual = xres;
    if (yres > 720)
    {
        var.yres_virtual = yres;
    }
    else
    {
        var.yres_virtual = yres * 2;
    }
    var.bits_per_pixel = 16;
    var.vmode = FB_VMODE_NONINTERLACED;
    var.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE | FB_ACTIVATE_ALL;
    var.red.offset = 11;
    var.red.length = 5;
    var.red.msb_right = 0;
    var.green.offset = 5;
    var.green.length = 6;
    var.green.msb_right = 0;
    var.blue.offset = 0;
    var.blue.length = 5;
    var.blue.msb_right = 0;

    // Set the proper resolution
    fb_set_var(hdmi_get_fb_info(), &var);
    return true;
}

void mirroring_cable_conn(struct hdmi_info *hdmi)
{
    HDMI_DBG("mirroring_cable_conn\n");

    settings.mirroringState = true;
    mirroring_setResolution(hdmi);
    hdmi_startMirroring(0);
    return;
}

void mirroring_cable_disconn(struct hdmi_info *hdmi)
{
    HDMI_DBG("mirroring_cable_disconn\n");
    hdmi_stopMirroring(0);
    settings.mirroringState = false;
    return;
}

bool mirroring_hdmi_enable_requested(void)
{
    hdmi_stopMirroring(1);
    return true;
}

bool mirroring_hdmi_disable_requested(void)
{
    // Make sure we're supposed to enable
    if (!settings.mirroringState)
        return true;

    // Check if HDMI is connected
    if (!hdmi_isCableConnected())
        return true;

    mirroring_setResolution(hdmi_get_hdmi_info());
    hdmi_startMirroring(1);
    mirroring_setResolution(hdmi_get_hdmi_info());
    return true;
}

void mirroring_getSettings(struct mirror_settings* pSettings)
{
    pSettings->rotation = settings.rotation;
    pSettings->scaling = settings.scaling;
    pSettings->overscanX = settings.overscanX;
    pSettings->overscanY = settings.overscanY;
    pSettings->audioOverHdmi = settings.audioOverHdmi;
    return;
}

void mirroring_setSettings(struct mirror_settings* pSettings)
{
    settings.rotation = pSettings->rotation;
    settings.scaling = pSettings->scaling;
    settings.overscanX = pSettings->overscanX;
    settings.overscanY = pSettings->overscanY;
    settings.audioOverHdmi = pSettings->audioOverHdmi;
    return;
}

