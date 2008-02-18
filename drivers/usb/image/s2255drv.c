/*
 * Sensoray 2255 USB Video for Linux driver
 *
 * Copyright (C) 2007-2008 by Sensoray Company Inc.
 *                            Dean Anderson
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License as
 *      published by the Free Software Foundation, version 2.
 *
 * Some video buffer code based on vivi driver:
 *
 * TODO: Incorporate videodev2 frame rate(FR) enumeration
 *       (currently experimental.)
 *
 *       2255 device supports 4 simultaneous channels.
 *       The channels are not "crossbar" inputs, they are physically
 *       attached to separate video decoders.
 *       Because of USB2.0 bandwidth limitations. There is only a
 *       certain amount of data which may be transferred at one time
 *       Because FR control is not in V4L yet, we may want to
 *       limit the cases:
 *       1) full size, color mode YUYV or YUV422P:
 *          2 video_devices allowed at full size.
 *       2) full or half size Grey scale:
 *          4 video_devices
 *       3) half size, color mode YUYV or YUV422P
 *          4 video_devices
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/pci.h>
#include <linux/random.h>
#include <linux/version.h>
#include <linux/mutex.h>
#include <linux/videodev2.h>
#include <linux/dma-mapping.h>
#ifdef CONFIG_VIDEO_V4L1_COMPAT
/* Include V4L1 specific functions. Should be removed soon */
#include <linux/videodev.h>
#endif
#include <linux/proc_fs.h>
#include <linux/interrupt.h>
#include <media/videobuf-vmalloc.h>
#include <media/v4l2-common.h>
#include <linux/kthread.h>
#include <linux/highmem.h>
#include <linux/freezer.h>
#include <linux/vmalloc.h>
#include <linux/usb.h>
#include "s2255drv.h"
#include "f2255usb.h" /* firmware in header file */

#define CUR_USB_FWVER 774 /* current cypress EEPROM firmware version */
static char MODULE_REVISION[] = "Ver.1.0.1";
#define S2255_MAJOR_VERSION 1
#define S2255_MINOR_VERSION 1
#define S2255_RELEASE 0
#define S2255_VERSION KERNEL_VERSION(S2255_MAJOR_VERSION, S2255_MINOR_VERSION, S2255_RELEASE)

/* vendor ids */
#define USB_S2255_VENDOR_ID   0x1943
#define USB_S2255_PRODUCT_ID  0x2255
#define S2255_NORMS (V4L2_STD_PAL_B | V4L2_STD_NTSC_M)
/* frame prefix size (sent once every frame) */
#define PREFIX_SIZE         512

/* Because the channels were physically printed on the box in
   reverse order than originally planned */
static unsigned long G_chnmap[MAX_CHANNELS]= {3,2,1,0};

static LIST_HEAD(s2255_devlist);

static int s2255_probe_v4l(struct s2255_dev *dev);
static void s2255_exit_v4l(struct s2255_dev *dev);
static int save_frame(struct s2255_dev *dev,
                          struct s2255_pipeinfo *pPipeInfo);

static int s2255_wait_frame_block(struct s2255_dev *dev, int chn);
static int s2255_wait_frame_noblock(struct s2255_dev *dev, int chn);

static int s2255_write_config(struct usb_device *udev, unsigned char *pbuf,
                              int size);
static int s2255_start_readpipe(struct s2255_dev *dev);
static void s2255_stop_readpipe(struct s2255_dev *dev);
static int s2255_create_sys_buffers( struct s2255_dev *dev,unsigned long chn);
static int s2255_release_sys_buffers( struct s2255_dev *dev,unsigned long chn);
static int s2255_board_init(struct s2255_dev *dev);
static int s2255_board_shutdown(struct s2255_dev *dev);

static long s2255_vendor_req(struct s2255_dev *dev, unsigned char req,
                             u16 index,
                             u16 val,void *buffer,
                             s32 buf_len, int bOut);
static int s2255_set_mode( struct s2255_dev *dev, unsigned long chn,
                           struct mode2255i *mode);

static int s2255_start_acquire(struct s2255_dev *dev, unsigned long chn);
static int s2255_stop_acquire(struct s2255_dev *dev, unsigned long chn);
static int restart_video_queue(struct s2255_dmaqueue *dma_q);
static void s2255_sleep( int ms);
static void planar422p_to_yuy2(  const unsigned char *in, unsigned char *out,
                                 int width, int height);

static void planar422p_to_rgb32(  const unsigned char *in, unsigned char *out,
                                  int width, int height, int rev_order);
static void planar422p_to_rgb24(  const unsigned char *in, unsigned char *out,
                                  int width, int height, int rev_order);
static void planar422p_to_rgb565(  unsigned char const *in, unsigned char *out, int width, int height, int rev_order);
static u32 get_transfer_size(struct mode2255i *mode);
static int s2255_get_fx2fw( struct s2255_dev *dev);

static int debug = 0;
int *s2255_debug=&debug;
/* Declare static vars that will be used as parameters */
static unsigned int vid_limit = 16;     /* Video memory limit, in Mb */

/* start video number */
static int video_nr = -1;               /* /dev/videoN, -1 for autodetect */



module_param(debug,int,0);
MODULE_PARM_DESC(debug, "Debug level(0-100) default 0");
module_param(vid_limit,int,0);
MODULE_PARM_DESC(vid_limit, "video memory limit(Mb)");
module_param(video_nr,int,0);
MODULE_PARM_DESC(video_nr, "start video minor(-1 default autodetect)");

/* device table */
static struct usb_device_id s2255_table [] = {
        { USB_DEVICE(USB_S2255_VENDOR_ID, USB_S2255_PRODUCT_ID) },
        { }                                     /* Terminating entry */
};
MODULE_DEVICE_TABLE (usb, s2255_table);


int norm_maxw(struct video_device *vdev )
{
        return (vdev->current_norm != V4L2_STD_PAL_B) ?
                LINE_SZ_4CIFS_NTSC : LINE_SZ_4CIFS_PAL;
}

int norm_maxh(struct video_device *vdev)
{
        return(vdev->current_norm != V4L2_STD_PAL_B ) ?
                (NUM_LINES_1CIFS_NTSC * 2) : (NUM_LINES_1CIFS_PAL * 2);
}

int norm_minw(struct video_device *vdev )
{
        return (vdev->current_norm != V4L2_STD_PAL_B) ?
                LINE_SZ_1CIFS_NTSC : LINE_SZ_1CIFS_PAL;
}

int norm_minh(struct video_device *vdev)
{
        return (vdev->current_norm != V4L2_STD_PAL_B) ?
                (NUM_LINES_1CIFS_NTSC) : (NUM_LINES_1CIFS_PAL);
}


#define dprintk(level,fmt, arg...)                                      \
        do {                                                            \
                if( *s2255_debug >= (level)) {                          \
                        printk(KERN_DEBUG "s2255: " fmt, ##arg);        \
                }                                                       \
        } while (0)


#define to_s2255_dev(d) container_of(d, struct s2255_dev, kref)

static DEFINE_MUTEX(usb_s2255_open_mutex);
static struct usb_driver s2255_driver;


/* kickstarts the firmware loading. from probe
 */
static void s2255_timer( unsigned long user_data)
{
        struct complete_data *data = (struct complete_data*) user_data;
        dprintk(100,"s2255 timer\n");
        if( usb_submit_urb( data->fw_urb, GFP_ATOMIC) < 0) {
                printk("can't submit urb\n");
                return;
        }
}

/* this loads the firmware asynchronously.
   Originally this was done synchroously in probe.
   But it is better to load it asynchronously here than block
   inside the probe function. Blocking inside probe affects boot time.
   FW loading is triggered by the timer in the probe function
*/
static void s2255_fwchunk_complete(struct urb *urb)
{
        struct complete_data *data = urb->context;
        struct usb_device *udev = urb->dev;
        int len;
        dprintk(100,"udev %p urb %p", udev, urb);
        if (urb->status) {
                printk("URB failed with status %d", urb->status);
                return;
        }
        if( data->fw_urb == NULL) {
                printk("early disconncect\n");
                return;
        }
#define CHUNK_SIZE 512
        /* all USB transfers must be done with continuous kernel memory.
           can't allocate more than 128k in current linux kernel, so
           upload the firmware in chunks
        */
        if( data->fw_loaded < data->fw_size) {
                len = (data->fw_loaded + CHUNK_SIZE) > data->fw_size ?
                        data->fw_size % CHUNK_SIZE : CHUNK_SIZE;
                dprintk(100,"completed len %d, loaded %d \n", len,
                        data->fw_loaded);
                memcpy( data->pfw_data, G_f2255usb + data->fw_loaded, len);

                usb_fill_bulk_urb( data->fw_urb, udev, usb_sndbulkpipe(udev, 2),
                                   data->pfw_data, CHUNK_SIZE,
                                   s2255_fwchunk_complete, data);

                if( usb_submit_urb( data->fw_urb, GFP_ATOMIC) < 0) {
                        printk("failed submit URB\n");
                        data->fw_state = FWSTATE_FAILED;
                        return;
                }
                data->fw_loaded+=len;
        } else {
                data->fw_state = FWSTATE_SUCCESS;
                printk( KERN_INFO "2255 firmware loaded successfully\n");
        }


        dprintk(100, "2255 complete done\n");
        return;

}



/* standard usb probe function */
static int s2255_probe
(struct usb_interface *interface, const struct usb_device_id *id)
{
        struct s2255_dev *dev = NULL;
        struct usb_host_interface *iface_desc;
        struct usb_endpoint_descriptor *endpoint;
        int i;
        int retval = -ENOMEM;
        printk(KERN_INFO "s2255: probe\n");
        /* allocate memory for our device state and initialize it to zero */
        dev = (struct s2255_dev *) kzalloc(sizeof(struct s2255_dev), GFP_KERNEL);


        if (dev == NULL) {
                err("s2255: out of memory");
                goto error;
        }

        /* grab usb_device and save it */
        dev->udev = usb_get_dev(interface_to_usbdev(interface));
        if( dev->udev == NULL) {
                printk("null usb device\n");
                goto error;
        }

        kref_init(&dev->kref);
        dprintk(1,"dev: %p, kref: %p udev %p interface %p\n", dev, &dev->kref,
               dev->udev, interface);
        dev->interface = interface;
        /* set up the endpoint information  */
        iface_desc = interface->cur_altsetting;
        printk("num endpoints %d\n" ,iface_desc->desc.bNumEndpoints);
        for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
                endpoint = &iface_desc->endpoint[i].desc;
                if (!dev->read_endpoint &&
                    ((endpoint->bEndpointAddress & USB_ENDPOINT_DIR_MASK)
                     == USB_DIR_IN) &&
                    ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)
                     == USB_ENDPOINT_XFER_BULK)) {
                        /* we found the bulk in endpoint */
                        dev->read_endpoint = endpoint->bEndpointAddress;
                }
        }

        if (!dev->read_endpoint ) {
                err("Could not find both bulk-in endpoint");
                goto error;
        }

        /* set intfdata */
        usb_set_intfdata(interface, dev);

        dprintk(100, "after intfdata %p\n", dev);

        /* initialize COUNTING semaphores */
        for( i=0;i< MAX_CHANNELS; i++) {
                sema_init( &dev->sem_frms[i], 0);
        }

        /* initialize device mutex */
        mutex_init(&dev->lock);

        init_timer( &dev->timer);
        dev->timer.function = s2255_timer;
        dev->fw_data = kzalloc( sizeof(struct complete_data), GFP_KERNEL);
        if( !dev->fw_data) {
                goto error;
        }

        dev->timer.data = (unsigned long) dev->fw_data;

        dev->fw_data->fw_size = sizeof(G_f2255usb) / sizeof(unsigned char);
        dev->fw_data->fw_urb = usb_alloc_urb(0, GFP_KERNEL);

        if( !dev->fw_data->fw_urb ) {
                printk("out of memory!\n");
                goto error;
        }
        dev->fw_data->pfw_data = kzalloc( CHUNK_SIZE, GFP_KERNEL);
        if(!dev->fw_data->pfw_data) {
                printk("out of mem\n");
                goto error;
        }

        /* load the first chunk */
        memcpy( dev->fw_data->pfw_data, G_f2255usb, CHUNK_SIZE);
        dev->fw_data->fw_loaded = CHUNK_SIZE;
        usb_fill_bulk_urb( dev->fw_data->fw_urb, dev->udev, usb_sndbulkpipe(dev->udev, 2),
                           dev->fw_data->pfw_data, CHUNK_SIZE, s2255_fwchunk_complete, dev->fw_data);
        /* loads v4l specific */
        s2255_probe_v4l( dev);
        /* load 2255 board specific */
        s2255_board_init( dev);

        printk("Sensoray 2255 successfully loaded\n");
        dprintk(4,"before probe done %p\n",dev);

        mod_timer( &dev->timer, jiffies + HZ);

        kref_get(&dev->kref);
        return 0;
error:
        return retval;
}


static void s2255_destroy(struct kref *kref)
{
        struct s2255_dev *dev = to_s2255_dev( kref);
        if( dev) {
                usb_put_dev(dev->udev);
        }

        dprintk(1,"s2255_destroy\n");
        kfree (dev);
}


/* disconnect routine.  when board is removed physically or with rmmod
 */
static void s2255_disconnect(struct usb_interface *interface)
{
        struct s2255_dev *dev = NULL;
        /* lock to prevent s2255_open() from racing s2255_disconnect() */
        mutex_lock( &usb_s2255_open_mutex);
        printk( KERN_INFO "s2255: disconnect interface %p\n", interface);
        dev = usb_get_intfdata(interface);
        s2255_board_shutdown(dev);
        if( dev->fw_data->fw_urb) {
                dprintk(2,"kill URB\n");
                usb_kill_urb( dev->fw_data->fw_urb);
                usb_free_urb( dev->fw_data->fw_urb);

        }
        s2255_exit_v4l( dev);
        if( dev->fw_data) {
                if( dev->fw_data->pfw_data) {
                        kfree(dev->fw_data->pfw_data);
                }
                kfree(dev->fw_data);
        }
        usb_set_intfdata(interface, NULL);
        kref_put(&dev->kref, s2255_destroy);
        mutex_unlock( &usb_s2255_open_mutex);
        info("s2255usb now disconnected\n");
}


/* Generate proc info.
 */
int s2255_read_procmem( char *buf, char **start, off_t offset, int count, int *eof, void *data )
{
        int len=0;
        /* Generate report heading. */
        len += sprintf( buf + len, "Sensoray 2255 drvr, version %s\n", MODULE_REVISION);

        len += sprintf( buf + len, "\n" );

        *eof = 1;
        return len;
}


static struct usb_driver s2255_driver = {
        .name =         "s2255",
        .probe =        s2255_probe,
        .disconnect =   s2255_disconnect,
        .id_table =     s2255_table,
};


static int __init usb_s2255_init(void)
{
        int result;

        /* Make public the function that supplies "proc" info to the system.
         */
        create_proc_read_entry( "s2255",
                                0,
                                NULL,
                                s2255_read_procmem,
                                NULL);

        /* register this driver with the USB subsystem */
        result = usb_register(&s2255_driver);

        if (result) {
                err("usb_register failed. Error number %d", result);
        }

        dprintk(2, "s2255_init: done\n");
        return result;
}

static void __exit usb_s2255_exit(void)
{
        usb_deregister(&s2255_driver);
}



static int s2255_got_frame( struct s2255_dev *dev, int chn)
{
        dprintk(2,"wakeup: %p channel: %d\n", &dev->sem_frms[chn],chn);
        up( &dev->sem_frms[chn]);
        return 0;
}

static int s2255_wait_frame_noblock( struct s2255_dev *dev, int chn)
{
        if(dev == NULL) {
                return -1;
        }
        dprintk(2,"wait frame: %p channel: %d\n", &dev->sem_frms[chn], chn);
        if(down_trylock( &dev->sem_frms[chn])) {
                dprintk(4, "wait_event: would block\n");
                return -1;
        }

        return 0;
}

static int s2255_wait_frame_block( struct s2255_dev *dev, int chn)
{
        int res;
        if(dev == NULL) {
                return -1;
        }
        dprintk(2,"wait frame: %p channel: %d\n", &dev->sem_frms[chn],chn);
        res = down_interruptible( &dev->sem_frms[chn]);

        return res;
}

/* buffer timeout.  Do not make this smaller than
   300ms.  The long timeout is required because the hardware
   internally restartswhen a new video source is plugged in.
 */
#define BUFFER_TIMEOUT msecs_to_jiffies(300)
/* initial startup timeout*/
#define BUFFER_TIMEOUT_INIT msecs_to_jiffies(900)


/* supported controls */
static struct v4l2_queryctrl s2255_qctrl[] = {
        {
                .id            = V4L2_CID_BRIGHTNESS,
                .type          = V4L2_CTRL_TYPE_INTEGER,
                .name          = "Brightness",
                .minimum       = -127,
                .maximum       = 128,
                .step          = 1,
                .default_value = 0,
                .flags         = 0,
        }, {
                .id            = V4L2_CID_CONTRAST,
                .type          = V4L2_CTRL_TYPE_INTEGER,
                .name          = "Contrast",
                .minimum       = 0,
                .maximum       = 255,
                .step          = 0x1,
                .default_value = DEF_CONTRAST,
                .flags         = 0,
        }, {
                .id            = V4L2_CID_SATURATION,
                .type          = V4L2_CTRL_TYPE_INTEGER,
                .name          = "Saturation",
                .minimum       = 0,
                .maximum       = 255,
                .step          = 0x1,
                .default_value = DEF_SATURATION,
                .flags         = 0,
        }, {
                .id            = V4L2_CID_HUE,
                .type          = V4L2_CTRL_TYPE_INTEGER,
                .name          = "Hue",
                .minimum       = 0,
                .maximum       = 255,
                .step          = 0x1,
                .default_value = DEF_HUE,
                .flags         = 0,
        }
};

static int qctl_regs[ARRAY_SIZE(s2255_qctrl)];


/* image formats.  Note RGB formats are software converted.
 *  because the 2255 transfers in YUV for maximum USB efficiency
 * in order to allow 2 full size color channels at full frame rate
 */
static const struct s2255_fmt formats[] = {
        {
                .name     = "4:2:2, planar, YUV422P",
                .fourcc   = V4L2_PIX_FMT_YUV422P,
                .depth    = 16
        },
        {
                .name     = "4:2:2, packed, YUYV",
                .fourcc   = V4L2_PIX_FMT_YUYV,
                .depth    = 16
        },
        {
                .name     = "BGR24",
                .fourcc   = V4L2_PIX_FMT_BGR24,
                .depth    = 24
        },
        {
                .name     = "RGB24",
                .fourcc   = V4L2_PIX_FMT_RGB24,
                .depth    = 24
        },
        {
                .name     = "BGR32",
                .fourcc   = V4L2_PIX_FMT_BGR32,
                .depth    = 32
        },
        {
                .name     = "RGB24",
                .fourcc   = V4L2_PIX_FMT_RGB32,
                .depth    = 32
        },
        {
                .name     = "RGB565",
                .fourcc   = V4L2_PIX_FMT_RGB565,
                .depth    = 16
        },
        {
                .name     = "RGB565 big endian",
                .fourcc   = V4L2_PIX_FMT_RGB565X,
                .depth    = 16
        },
        {
                .name     = "8bpp GREY",
                .fourcc   = V4L2_PIX_FMT_GREY,
                .depth    = 8
        },
};


static const struct s2255_fmt *format_by_fourcc(int fourcc)
{
        unsigned int i;

        for (i = 0; i < ARRAY_SIZE(formats); i++) {
                if (-1 == formats[i].fourcc)
                        continue;
                if (formats[i].fourcc == fourcc)
                        return formats+i;
        }
        return NULL;
}




/* video buffer thread and vmalloc based partly on VIVI driver which is
 *          Copyright (c) 2006 by
 *                  Mauro Carvalho Chehab <mchehab--a.t--infradead.org>
 *                  Ted Walther <ted--a.t--enumera.com>
 *                  John Sokol <sokol--a.t--videotechnology.com>
 *                  http://v4l.videotechnology.com/
 *
 */
static void s2255_fillbuff(struct s2255_dev *dev,struct s2255_buffer *buf,int chn)
{
        int pos=0;
        struct timeval ts;
        const char *tmpbuf;
        char *vbuf = videobuf_to_vmalloc(&buf->vb);
        unsigned long last_frame;
        struct framei *frm;
        last_frame = dev->last_frame[chn];
        if ((last_frame != -1) && (vbuf != NULL)) {
                frm = &dev->buffer[chn].frame[last_frame];
                tmpbuf = (const char *) dev->buffer[chn].frame[last_frame].lpvbits;
                if ( buf->fmt->fourcc == V4L2_PIX_FMT_YUYV ) {
                        planar422p_to_yuy2((const unsigned char *) tmpbuf,
                                           vbuf, buf->vb.width, buf->vb.height);
                } else if ( buf->fmt->fourcc == V4L2_PIX_FMT_GREY ) {
                        memcpy(vbuf,tmpbuf, buf->vb.width*buf->vb.height);
                } else if ( buf->fmt->fourcc == V4L2_PIX_FMT_YUV422P) {
                        memcpy(vbuf,tmpbuf, buf->vb.width*buf->vb.height*2);
                } else if (buf->fmt->fourcc == V4L2_PIX_FMT_RGB24) {
                        planar422p_to_rgb24(tmpbuf, vbuf, buf->vb.width,
                                            buf->vb.height,0);
                } else if (buf->fmt->fourcc == V4L2_PIX_FMT_BGR24) {
                        planar422p_to_rgb24(tmpbuf, vbuf, buf->vb.width,
                                            buf->vb.height,1);
                } else if (buf->fmt->fourcc == V4L2_PIX_FMT_RGB32) {
                        planar422p_to_rgb32(tmpbuf, vbuf, buf->vb.width,
                                            buf->vb.height,0);
                } else if (buf->fmt->fourcc == V4L2_PIX_FMT_BGR32) {
                        planar422p_to_rgb32(tmpbuf, vbuf, buf->vb.width,
                                            buf->vb.height,1);
                } else if (buf->fmt->fourcc == V4L2_PIX_FMT_RGB565) {
                        planar422p_to_rgb565(tmpbuf, vbuf, buf->vb.width,
                                            buf->vb.height,0);
                } else if (buf->fmt->fourcc == V4L2_PIX_FMT_RGB565X) {
                        planar422p_to_rgb565(tmpbuf, vbuf, buf->vb.width,
                                            buf->vb.height,1);
                } else {
                        printk( KERN_DEBUG "s2255: unknown format?\n");
                }
                dev->last_frame[chn]  = -1;
                /* done with the frame, free it */
                frm->ulState = 0;
                dprintk(4,"freeing buffer\n");
        } else {
                printk( KERN_ERR "s2255: =======no frame\n");
                return;

        }
        dprintk(2,"s2255fill at : Buffer 0x%08lx size= %d\n",
                (unsigned long)vbuf,pos);
        /* tell v4l buffer was filled */
        buf->vb.state = VIDEOBUF_DONE;
        buf->vb.field_count++;
        do_gettimeofday(&ts);
        buf->vb.ts = ts;
        list_del(&buf->vb.queue);
        wake_up(&buf->vb.done);
}




static int s2255_thread(void *data)
{
        struct s2255_dmaqueue  *dma_q=data;
        struct s2255_dev       *dev=dma_q->dev;
        struct s2255_buffer    *buf;
        int chn = dma_q->channel;
        int res;
        int i;
        int tmp;
        dprintk(1,"thread started %d\n", dma_q->channel);
        /* count down the semaphore to zero
           when acquisition restarted.
        */
        tmp = s2255_wait_frame_noblock( dev, chn);
        while(tmp == 0 ) {
                tmp = s2255_wait_frame_noblock(dev, chn);
        }

        /* initialize the states */
        dev->b_acquire[chn] = 1;
        dev->cur_frame[chn] = 0;
        dev->last_frame[chn] = -1;
        dev->bad_payload[chn] = 0;

        for( i=0;i<SYS_FRAMES;i++) {
                dev->buffer[chn].frame[i].ulState = 0;
                dev->buffer[chn].frame[i].cur_size = 0;
        }

        /* start the frame timer */
        mod_timer(&dma_q->timeout, jiffies+BUFFER_TIMEOUT_INIT);
        for (;;) {
                dprintk(4,"before wait frame ===========\n");
                res = s2255_wait_frame_block( dev, dma_q->channel);
                dprintk(4,"after wait frame ===========\n");
                if (list_empty(&dma_q->active)) {
                        dprintk(1,"No active queue to serve\n");
                        break;
                }

                buf = list_entry(dma_q->active.next,
                                 struct s2255_buffer, vb.queue);

                if(!waitqueue_active(&buf->vb.done)) {
                        /* no one active */
                        mod_timer(&dma_q->timeout, jiffies+BUFFER_TIMEOUT);
                        if (kthread_should_stop()) {
                                break;
                        }
                        continue;
                }
                do_gettimeofday(&buf->vb.ts);
                dprintk(100,"[%p/%d] wakeup\n",buf,buf->vb.i);
                s2255_fillbuff(dev,buf,dma_q->channel);
                mod_timer(&dma_q->timeout, jiffies+BUFFER_TIMEOUT);
                if (kthread_should_stop()) {
                        break;
                }
                dprintk(3,"thread tick \n");
        }
        dprintk(1, "thread: exit %d\n", dma_q->channel);
        /* tell read complete to not bother saving frames for this channel */
        dev->b_acquire[chn] = 0;

        return 0;
}


static int s2255_start_thread(struct s2255_dmaqueue  *dma_q)
{
        dma_q->frame=0;
        dprintk(1,"%s[%d]\n",__FUNCTION__,dma_q->channel);
        dma_q->kthread = kthread_run(s2255_thread, dma_q, "s2255");
        if (IS_ERR(dma_q->kthread)) {
                printk(KERN_ERR "s2255: kernel_thread() failed\n");
                return PTR_ERR(dma_q->kthread);
        }
        /* Wakes thread */
        wake_up_interruptible(&dma_q->wq);

        dprintk(1,"returning from %s\n",__FUNCTION__);
        return 0;
}

static void s2255_stop_thread(struct s2255_dmaqueue  *dma_q)
{
        dprintk(1,"%s[%d]\n",__FUNCTION__,dma_q->channel);
        /* unblock the kthread */
        dprintk(1,"stop thread channel %d\n", dma_q->channel);
        /* wakeup the thread in case it's waiting */
        s2255_got_frame( dma_q->dev, dma_q->channel);
        /* shutdown control thread */
         if (dma_q->kthread) {
                kthread_stop(dma_q->kthread);
                dma_q->kthread=NULL;
        }

        dprintk(1,"%s exiting\n",__FUNCTION__);
        return;
}

static int restart_video_queue(struct s2255_dmaqueue *dma_q)
{
        struct s2255_buffer *buf, *prev;
        struct list_head *item;

        dprintk(1,"%s dma_q=0x%08lx chan %d\n",__FUNCTION__,(unsigned long)dma_q, dma_q->channel);

        if (!list_empty(&dma_q->active)) {
                buf = list_entry(dma_q->active.next, struct s2255_buffer, vb.queue);
                dprintk(2,"restart_queue [%p/%d]: restart dma\n",
                        buf, buf->vb.i);

                dprintk(1,"Restarting video dma\n");
                s2255_stop_thread(dma_q);
                /* line below from vivi driver.
                   was commented out there also.. */
                // s2255_start_thread(dma_q);

                /* cancel all outstanding capture requests */
                list_for_each(item,&dma_q->active) {
                        buf = list_entry(item, struct s2255_buffer, vb.queue);

                        list_del(&buf->vb.queue);
                        buf->vb.state = VIDEOBUF_ERROR;
                        wake_up(&buf->vb.done);
                }
                mod_timer(&dma_q->timeout, jiffies+BUFFER_TIMEOUT);

                return 0;
        }

        prev = NULL;
        for (;;) {
                if (list_empty(&dma_q->queued)) {
                        dprintk(1,"exiting nothing queued\n");
                        return 0;
                }
                buf = list_entry(dma_q->queued.next, struct s2255_buffer, vb.queue);
                if (NULL == prev) {
                        list_del(&buf->vb.queue);
                        list_add_tail(&buf->vb.queue,&dma_q->active);

                        dprintk(1,"Restarting video dma\n");
                        s2255_stop_thread(dma_q);
                        s2255_start_thread(dma_q);

                        buf->vb.state = VIDEOBUF_ACTIVE;
                        mod_timer(&dma_q->timeout, jiffies+BUFFER_TIMEOUT);
                        dprintk(2,"[%p/%d] restart_queue - first active\n",
                                buf,buf->vb.i);

                } else if (prev->vb.width  == buf->vb.width  &&
                           prev->vb.height == buf->vb.height &&
                           prev->fmt       == buf->fmt) {
                        list_del(&buf->vb.queue);
                        list_add_tail(&buf->vb.queue,&dma_q->active);
                        buf->vb.state = VIDEOBUF_ACTIVE;
                        dprintk(2,"[%p/%d] restart_queue - move to active\n",
                                buf,buf->vb.i);
                } else {
                        return 0;
                }
                prev = buf;
        }

}

static void s2255_vid_timeout(unsigned long data)
{
        struct s2255_dmaqueue *vidq = (struct s2255_dmaqueue *) data;
        struct s2255_buffer   *buf;

        dprintk(1,"[%d]vid timeout %p\n",vidq->channel,vidq);
        while (!list_empty(&vidq->active)) {
                buf = list_entry(vidq->active.next, struct s2255_buffer, vb.queue);
                list_del(&buf->vb.queue);
                buf->vb.state = VIDEOBUF_ERROR;
                wake_up(&buf->vb.done);
        }
        restart_video_queue(vidq);
}

/* ------------------------------------------------------------------
   Videobuf operations
   ------------------------------------------------------------------*/
static int
buffer_setup(struct videobuf_queue *vq, unsigned int *count, unsigned int *size)
{
        struct s2255_fh *fh = vq->priv_data;

        *size = fh->width*fh->height * (fh->fmt->depth >> 3) ;

        if (0 == *count) {
                *count = 32;
        }

        while (*size * *count > vid_limit * 1024 * 1024)
                (*count)--;

        return 0;
}

static void free_buffer(struct videobuf_queue *vq, struct s2255_buffer *buf)
{
        dprintk(4,"%s\n",__FUNCTION__);

        if( vq == NULL) {
                dprintk(4,"null vq\n");
                return;
        }
        if( buf == NULL) {
                dprintk(4,"null buffer\n");
                return;
        }

        if (in_interrupt()) {
                dprintk(4,"in interrupt\n");
                /* BUG(); */
        }
        videobuf_waiton(&buf->vb,0,0);
        videobuf_vmalloc_free(&buf->vb);
        buf->vb.state = VIDEOBUF_NEEDS_INIT;
}



static int
buffer_prepare(struct videobuf_queue *vq, struct videobuf_buffer *vb,
               enum v4l2_field field)
{
        struct s2255_fh     *fh  = vq->priv_data;
        struct s2255_buffer *buf = container_of(vb,struct s2255_buffer,vb);
        int rc, init_buffer = 0;
        dprintk(4,"%s, field=%d\n",__FUNCTION__,field);
        if( fh->fmt == NULL) {
                return -EINVAL;
        }
        if ((fh->width  < norm_minw(fh->dev->vdev[fh->channel])) ||
            (fh->width  > norm_maxw(fh->dev->vdev[fh->channel])) ||
            (fh->height < norm_minh(fh->dev->vdev[fh->channel])) ||
            (fh->height > norm_maxh(fh->dev->vdev[fh->channel]))) {
                dprintk(4, "invalid buffer prepare\n");
                return -EINVAL;
        }

        buf->vb.size = fh->width*fh->height*(fh->fmt->depth >> 3);

        if (0 != buf->vb.baddr  &&  buf->vb.bsize < buf->vb.size) {
                dprintk(4, "invalid buffer prepare\n");
                return -EINVAL;
        }

        if (buf->fmt       != fh->fmt    ||
            buf->vb.width  != fh->width  ||
            buf->vb.height != fh->height ||
            buf->vb.field  != field) {
                buf->fmt       = fh->fmt;
                buf->vb.width  = fh->width;
                buf->vb.height = fh->height;
                buf->vb.field  = field;
                init_buffer = 1;
        }

        if (VIDEOBUF_NEEDS_INIT == buf->vb.state) {
                if (0 != (rc = videobuf_iolock(vq,&buf->vb,NULL)))
                        goto fail;
        }

        buf->vb.state = VIDEOBUF_PREPARED;
        return 0;
fail:
        free_buffer(vq,buf);
        return rc;
}

static void
buffer_queue(struct videobuf_queue *vq, struct videobuf_buffer *vb)
{
        struct s2255_buffer    *buf     = container_of(vb,struct s2255_buffer,vb);
        struct s2255_fh        *fh      = vq->priv_data;
        struct s2255_dev       *dev     = fh->dev;
        struct s2255_dmaqueue  *vidq    = &dev->vidq[fh->channel];
        struct s2255_buffer    *prev;

        if (!list_empty(&vidq->queued)) {
                dprintk(1,"adding vb queue=0x%08lx\n",(unsigned long)&buf->vb.queue);
                list_add_tail(&buf->vb.queue,&vidq->queued);
                buf->vb.state = VIDEOBUF_QUEUED;
                dprintk(2,"[%p/%d] buffer_queue - append to queued\n",
                        buf, buf->vb.i);
        } else if (list_empty(&vidq->active)) {
                list_add_tail(&buf->vb.queue,&vidq->active);
                s2255_start_thread(vidq);
                buf->vb.state = VIDEOBUF_ACTIVE;
                mod_timer(&vidq->timeout, jiffies+BUFFER_TIMEOUT);
                dprintk(2,"[%p/%d] buffer_queue - first active\n",
                        buf, buf->vb.i);

        } else {
                prev = list_entry(vidq->active.prev, struct s2255_buffer, vb.queue);
                if (prev->vb.width  == buf->vb.width  &&
                    prev->vb.height == buf->vb.height &&
                    prev->fmt       == buf->fmt) {
                        list_add_tail(&buf->vb.queue,&vidq->active);
                        buf->vb.state = VIDEOBUF_ACTIVE;
                        dprintk(2,"[%p/%d] buffer_queue - append to active\n",
                                buf, buf->vb.i);

                } else {
                        list_add_tail(&buf->vb.queue,&vidq->queued);
                        buf->vb.state = VIDEOBUF_QUEUED;
                        dprintk(2,"[%p/%d] buffer_queue - first queued\n",
                                buf, buf->vb.i);
                }
        }
}

static void buffer_release(struct videobuf_queue *vq, struct videobuf_buffer *vb)
{
        struct s2255_buffer   *buf  = container_of(vb,struct s2255_buffer,vb);
        struct s2255_fh       *fh   = vq->priv_data;
        struct s2255_dev      *dev  = (struct s2255_dev*)fh->dev;
        struct s2255_dmaqueue *vidq = &dev->vidq[fh->channel];

        dprintk(1,"%s\n",__FUNCTION__);
        s2255_stop_thread(vidq);
        free_buffer(vq,buf);
}


static struct videobuf_queue_ops s2255_video_qops = {
        .buf_setup      = buffer_setup,
        .buf_prepare    = buffer_prepare,
        .buf_queue      = buffer_queue,
        .buf_release    = buffer_release,
};

/* ------------------------------------------------------------------
   IOCTL handling
   ------------------------------------------------------------------*/


static int res_get(struct s2255_dev *dev, struct s2255_fh *fh)
{
        /* is it free? */
        mutex_lock(&dev->lock);
        if (dev->resources[fh->channel]) {
                /* no, someone else uses it */
                mutex_unlock(&dev->lock);
                return 0;
        }
        /* it's free, grab it */
        dev->resources[fh->channel] = 1;
        dprintk(1,"res: get\n");
        mutex_unlock(&dev->lock);
        return 1;
}

static int res_locked(struct s2255_dev *dev, struct s2255_fh *fh)
{
        return (dev->resources[fh->channel]);
}

static void res_free(struct s2255_dev *dev, struct s2255_fh *fh)
{
        dev->resources[fh->channel] = 0;
        dprintk(1,"res: put\n");
}

/* ------------------------------------------------------------------
   IOCTL vidioc handling
   ------------------------------------------------------------------*/
static int vidioc_querycap (struct file *file, void  *priv,
                            struct v4l2_capability *cap)
{
        strcpy(cap->driver, "s2255");
        strcpy(cap->card, "s2255");
        cap->version = S2255_VERSION;
        cap->capabilities =     V4L2_CAP_VIDEO_CAPTURE |
                V4L2_CAP_STREAMING     |
                V4L2_CAP_READWRITE;
        return 0;
}

static int vidioc_enum_fmt_cap (struct file *file, void  *priv,
                                struct v4l2_fmtdesc *f)
{
        int index = 0;
        if (f) {
                index = f->index;
        }

        if (index >= ARRAY_SIZE(formats)) {
                return -EINVAL;
        }

        dprintk(4,"name %s\n", formats[index].name);
        strlcpy(f->description,formats[index].name,sizeof(f->description));
        f->pixelformat = formats[index].fourcc;
        return 0;
}

static int vidioc_g_fmt_cap (struct file *file, void *priv,
                             struct v4l2_format *f)
{
        struct s2255_fh  *fh=priv;

        f->fmt.pix.width        = fh->width;
        f->fmt.pix.height       = fh->height;
        f->fmt.pix.field        = fh->vb_vidq.field;
        f->fmt.pix.pixelformat  = fh->fmt->fourcc;
        f->fmt.pix.bytesperline = f->fmt.pix.width * (fh->fmt->depth >> 3);
        f->fmt.pix.sizeimage = f->fmt.pix.height * f->fmt.pix.bytesperline;

        return (0);
}

static int vidioc_try_fmt_cap (struct file *file, void *priv,
                               struct v4l2_format *f)
{
        const struct s2255_fmt *fmt;
        enum v4l2_field field;
        struct s2255_fh  *fh = priv;
        struct s2255_dev *dev = fh->dev;
        int is_ntsc;

        is_ntsc = (dev->vdev[fh->channel]->current_norm != V4L2_STD_PAL_B) ? 1 : 0;

        fmt=format_by_fourcc( f->fmt.pix.pixelformat);
        if (fmt == NULL) {
                return -EINVAL;
        }

        field = f->fmt.pix.field;

        if (field == V4L2_FIELD_ANY) {
                field=V4L2_FIELD_SEQ_TB;
        } else if (V4L2_FIELD_INTERLACED != field) {
                dprintk(1,"Field type invalid.\n");
                return -EINVAL;
        }

        dprintk(4,"try format %d \n", is_ntsc);
        /* supports 3 sizes. see s2255drv.h */
        dprintk(50,"width test %d, height %d\n",
                f->fmt.pix.width, f->fmt.pix.height);
        if( is_ntsc) {
                /* NTSC */
                if (f->fmt.pix.height >= NUM_LINES_1CIFS_NTSC*2 ) {
                        f->fmt.pix.height = NUM_LINES_1CIFS_NTSC*2;
                        field = V4L2_FIELD_INTERLACED;
                } else {
                        f->fmt.pix.height = NUM_LINES_1CIFS_NTSC;
                }


                if (f->fmt.pix.width >= LINE_SZ_4CIFS_NTSC) {
                        f->fmt.pix.width = LINE_SZ_4CIFS_NTSC;
                } else if (f->fmt.pix.width >= LINE_SZ_2CIFS_NTSC) {
                        f->fmt.pix.width = LINE_SZ_2CIFS_NTSC;
                } else if (f->fmt.pix.width >= LINE_SZ_1CIFS_NTSC) {
                        f->fmt.pix.width = LINE_SZ_1CIFS_NTSC;
                } else {
                        f->fmt.pix.width = LINE_SZ_1CIFS_NTSC;
                }
        } else {
                /* PAL */
                if (f->fmt.pix.height >= NUM_LINES_1CIFS_PAL*2 ) {
                        f->fmt.pix.height = NUM_LINES_1CIFS_PAL*2;
                        field = V4L2_FIELD_INTERLACED;
                } else {
                        f->fmt.pix.height = NUM_LINES_1CIFS_PAL;
                }
                if (f->fmt.pix.width >= LINE_SZ_4CIFS_PAL) {
                        dprintk(50,"pal 704\n");
                        f->fmt.pix.width = LINE_SZ_4CIFS_PAL;
                } else if (f->fmt.pix.width >= LINE_SZ_2CIFS_PAL) {
                        dprintk(50,"pal 352A\n");
                        f->fmt.pix.width = LINE_SZ_2CIFS_PAL;
                } else if (f->fmt.pix.width >= LINE_SZ_1CIFS_PAL) {
                        dprintk(50,"pal 352B\n");
                        f->fmt.pix.width = LINE_SZ_1CIFS_PAL;
                } else {
                        dprintk(50,"pal 352C\n");
                        f->fmt.pix.width = LINE_SZ_1CIFS_PAL;
                }
        }

        dprintk(50,"width %d height %d field %d \n", f->fmt.pix.width,
               f->fmt.pix.height, f->fmt.pix.field);
        f->fmt.pix.field = field;
        f->fmt.pix.bytesperline =
                (f->fmt.pix.width * fmt->depth) >> 3;
        f->fmt.pix.sizeimage =
                f->fmt.pix.height * f->fmt.pix.bytesperline;

        return 0;
}

/*FIXME: This seems to be generic enough to be at videodev2 */
static int vidioc_s_fmt_cap (struct file *file, void *priv,
                             struct v4l2_format *f)
{
        struct s2255_fh  *fh=priv;
        const struct s2255_fmt *fmt;
        int ret = vidioc_try_fmt_cap(file,fh,f);
        int norm;
        if (ret < 0) {
                return (ret);
        }
        fmt=format_by_fourcc( f->fmt.pix.pixelformat);
        if (fmt == NULL) {
                return -EINVAL;
        }

        fh->fmt           = fmt;
        fh->width         = f->fmt.pix.width;
        fh->height        = f->fmt.pix.height;
        fh->vb_vidq.field = f->fmt.pix.field;
        fh->type          = f->type;

        norm = norm_minw( fh->dev->vdev[fh->channel]);
        if( fh->width > norm_minw(fh->dev->vdev[fh->channel])) {
                if( fh->height > norm_minh(fh->dev->vdev[fh->channel])) {
                        fh->dev->mode[fh->channel].scale = SCALE_4CIFS;
                } else {
                        fh->dev->mode[fh->channel].scale = SCALE_2CIFS;
                }
        } else {
                fh->dev->mode[fh->channel].scale = SCALE_1CIFS;
        }

        /* color mode */
        if( fh->fmt->fourcc == V4L2_PIX_FMT_GREY) {
                fh->dev->mode[fh->channel].color = COLOR_Y8;
        } else if( fh->fmt->fourcc == V4L2_PIX_FMT_YUV422P) {
                fh->dev->mode[fh->channel].color = COLOR_YUVPL;
        } else if( fh->fmt->fourcc == V4L2_PIX_FMT_YUYV) {
                /* Note: software conversion from YUV422P to YUYV */
                fh->dev->mode[fh->channel].color = COLOR_YUVPK;
        } else if ( (fh->fmt->fourcc == V4L2_PIX_FMT_RGB24) ||
                    ( fh->fmt->fourcc == V4L2_PIX_FMT_BGR24) ||
                    ( fh->fmt->fourcc == V4L2_PIX_FMT_RGB32) ||
                    ( fh->fmt->fourcc == V4L2_PIX_FMT_RGB565) ||
                    ( fh->fmt->fourcc == V4L2_PIX_FMT_RGB565X) ||
                    ( fh->fmt->fourcc == V4L2_PIX_FMT_BGR32)) {
                /* Note:software conversion from YUV422P to RGB(s) */
                dprintk(2, "mode supported with software conversion.\n");
                dprintk(2, "for lower CPU usage, use V4L2_PIX_FMT_YUV422P"
                        "V4L2_PIX_FMT_YUVV(minimal software reordering) or"
                        " V4L2_PIX_FMT_GREY\n");
                fh->dev->mode[fh->channel].color = COLOR_YUVPL;
        }
        return (0);
}

static int vidioc_reqbufs (struct file *file, void *priv, struct v4l2_requestbuffers *p)
{
        struct s2255_fh  *fh=priv;

        return (videobuf_reqbufs(&fh->vb_vidq, p));
}

static int vidioc_querybuf (struct file *file, void *priv, struct v4l2_buffer *p)
{
        struct s2255_fh  *fh=priv;

        return (videobuf_querybuf(&fh->vb_vidq, p));
}

static int vidioc_qbuf (struct file *file, void *priv, struct v4l2_buffer *p)
{
        struct s2255_fh  *fh=priv;

        return (videobuf_qbuf(&fh->vb_vidq, p));
}

static int vidioc_dqbuf (struct file *file, void *priv, struct v4l2_buffer *p)
{
        struct s2255_fh  *fh=priv;

        return (videobuf_dqbuf(&fh->vb_vidq, p,
                               file->f_flags & O_NONBLOCK));
}

#ifdef CONFIG_VIDEO_V4L1_COMPAT
static int vidiocgmbuf (struct file *file, void *priv, struct video_mbuf *mbuf)
{
        struct s2255_fh  *fh=priv;
        struct videobuf_queue *q=&fh->vb_vidq;
        struct v4l2_requestbuffers req;
        unsigned int i;
        int ret;

        req.type   = q->type;
        req.count  = 8;
        req.memory = V4L2_MEMORY_MMAP;
        ret = videobuf_reqbufs(q,&req);
        if (ret < 0)
                return (ret);

        mbuf->frames = req.count;
        mbuf->size   = 0;
        for (i = 0; i < mbuf->frames; i++) {
                mbuf->offsets[i]  = q->bufs[i]->boff;
                mbuf->size       += q->bufs[i]->bsize;
        }
        return (0);
}
#endif

static int vidioc_streamon(struct file *file, void *priv, enum v4l2_buf_type i)
{
        int res;
        struct s2255_fh  *fh=priv;
        struct s2255_dev *dev    = fh->dev;
        struct mode2255i *mode;

        if (fh->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
                printk("invalid fh type0\n");
                return -EINVAL;
        }

        if (i != fh->type) {
                printk("invalid fh type1\n");
                return -EINVAL;
        }

        if (!res_get(dev,fh)) {
                printk("res get busy\n");
                return -EBUSY;
        }
        /* send a set mode command everytime with restart.
           in case we switch resolutions or other parameters */
        mode = &dev->mode[fh->channel];
        mode->restart = 1;
        dprintk(4, "videoc_streamon\n");
        s2255_set_mode(dev, fh->channel, mode);
        mode->restart = 0;
        s2255_start_acquire(dev, fh->channel);
        res = videobuf_streamon(&fh->vb_vidq);
        return res;
}

static int vidioc_streamoff(struct file *file, void *priv, enum v4l2_buf_type i)
{
        int res;
        struct s2255_fh  *fh = priv;
        struct s2255_dev *dev = fh->dev;
        dprintk(1, "[%d]videobuf stream off\n", fh->channel);


        if (fh->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
                printk("invalid fh type0\n");
                return -EINVAL;
        }

        if (i != fh->type) {
                printk("invalid fh type1\n");
                return -EINVAL;
        }

        s2255_stop_acquire(dev, fh->channel);
        res = videobuf_streamoff(&fh->vb_vidq);
        res_free(dev,fh);
        return res;
}

static int vidioc_s_std (struct file *file, void *priv, v4l2_std_id *i)
{
        struct s2255_fh  *fh = priv;
        struct s2255_dev *dev = fh->dev;
        struct mode2255i *mode;
        mode = &dev->mode[fh->channel];
        if (*i == V4L2_STD_NTSC_M ) {
                dprintk(4,"vidioc_s_std NTSC\n");
                mode->format = FORMAT_NTSC;
        } else if (*i == V4L2_STD_PAL_B) {
                dprintk(4,"vidioc_s_std PAL\n");
                mode->format = FORMAT_PAL;
        } else {
                return -EINVAL;
        }
        return 0;
}


/* Sensoray 2255 is a multiple channel capture device.
   It does not have a "crossbar" of inputs.
   We use one V4L device per channel. The user must
   be aware that certain combinations are not allowed.
   For instance, you cannot do full FPS on more than 2 channels(2 videodevs)
   at once in color(you can do full fps on 4 channels with greyscale.
*/
static int vidioc_enum_input (struct file *file, void *priv,
                              struct v4l2_input *inp)
{
        if (inp->index != 0) {
                return -EINVAL;
        }
        inp->type = V4L2_INPUT_TYPE_CAMERA;
        inp->std = S2255_NORMS;
        strcpy(inp->name,"Camera");
        return (0);
}

static int vidioc_g_input (struct file *file, void *priv, unsigned int *i)
{
        *i = 0;
        return (0);
}
static int vidioc_s_input (struct file *file, void *priv, unsigned int i)
{
        if (i > 0) {
                return -EINVAL;
        }

        return (0);
}

/* --- controls ---------------------------------------------- */
static int vidioc_queryctrl (struct file *file, void *priv,
                             struct v4l2_queryctrl *qc)
{
        int i;

        for (i = 0; i < ARRAY_SIZE(s2255_qctrl); i++)
                if (qc->id && qc->id == s2255_qctrl[i].id) {
                        memcpy(qc, &(s2255_qctrl[i]),
                               sizeof(*qc));
                        return (0);
                }

        return -EINVAL;
}

static int vidioc_g_ctrl (struct file *file, void *priv,
                          struct v4l2_control *ctrl)
{
        int i;

        for (i = 0; i < ARRAY_SIZE(s2255_qctrl); i++)
                if (ctrl->id == s2255_qctrl[i].id) {
                        ctrl->value=qctl_regs[i];
                        return (0);
                }

        return -EINVAL;
}

static int vidioc_s_ctrl (struct file *file, void *priv,
                          struct v4l2_control *ctrl)
{
        int i;
        struct s2255_fh  *fh = priv;
        struct s2255_dev *dev = fh->dev;
        struct mode2255i *mode;
        mode = &dev->mode[fh->channel];
        dprintk(4, "vidioc_s_ctrl\n");
        for (i = 0; i < ARRAY_SIZE(s2255_qctrl); i++) {
                if (ctrl->id == s2255_qctrl[i].id) {
                        if (ctrl->value <
                            s2255_qctrl[i].minimum
                            || ctrl->value >
                            s2255_qctrl[i].maximum) {
                                return (-ERANGE);
                        }
                        qctl_regs[i]=ctrl->value;
                        /* update the mode to the corresponding value */
                        if (ctrl->id == V4L2_CID_BRIGHTNESS) {
                                mode->bright = ctrl->value;
                        } else if (ctrl->id == V4L2_CID_CONTRAST) {
                                mode->contrast = ctrl->value;
                        } else if (ctrl->id == V4L2_CID_HUE) {
                                mode->hue = ctrl->value;
                        } else if (ctrl->id == V4L2_CID_SATURATION) {
                                mode->saturation = ctrl->value;
                        }
#if 1
                        mode->restart = 0;
                        /* set mode here.  Note: stream does not need restarted.
                           some V4L programs restart stream unnecessarily
                           after a s_crtl.
                        */
                        s2255_set_mode( dev, fh->channel, mode);
#endif
                        return 0;
                }
        }
        return -EINVAL;
}


static int s2255_open_v4l(struct inode *inode, struct file *file)
{
        int minor = iminor(inode);
        struct s2255_dev *h,*dev = NULL;
        struct s2255_fh *fh;
        struct list_head *list;
        enum v4l2_buf_type type = 0;
        int i=0;
        int cur_channel=-1;

        printk(KERN_DEBUG "s2255: open called (minor=%d)\n",minor);
        list_for_each(list,&s2255_devlist) {
                h = list_entry(list, struct s2255_dev, s2255_devlist);
                for (i=0;i<MAX_CHANNELS;i++) {
                        if (h->vdev[i]->minor == minor) {
                                cur_channel = i;
                                dev  = h;
                                type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                        }
                }
        }

        if((NULL == dev) || (cur_channel == -1)) {
                dprintk(1, "s2255: openv4l no dev\n");
                return -ENODEV;
        }

        mutex_lock( &usb_s2255_open_mutex);
        if( dev->fw_data->fw_state == FWSTATE_FAILED) {
                err("2255 firmware wasn't loaded\n");
                mutex_unlock( &usb_s2255_open_mutex);
                return -ENODEV;
        }

        if( dev->fw_data->fw_state == FWSTATE_NOTLOADED) {
                err("2255 firmware loading.( or failed)\n");
                mutex_unlock( &usb_s2255_open_mutex);
                return -EAGAIN;
        }

        dev->users[cur_channel]++;

        if (dev->users[cur_channel]>1) {
                dev->users[cur_channel]--;
                printk("one user at a time\n");
                mutex_unlock( &usb_s2255_open_mutex);
                return -EAGAIN;
        }

        dprintk(1,"open minor=%d type=%s users=%d\n",
                minor,v4l2_type_names[type],dev->users[cur_channel]);

        /* allocate + initialize per filehandle data */
        fh = kzalloc(sizeof(*fh),GFP_KERNEL);
        if (NULL == fh) {
                dev->users[cur_channel]--;
                mutex_unlock( &usb_s2255_open_mutex);
                return -ENOMEM;
        }

        file->private_data = fh;
        fh->dev      = dev;
        fh->type     = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fh->fmt      = &formats[0];
        /* default 1CIF NTSC */
        fh->width    = 640;
        fh->height   = 480;
        fh->channel  = cur_channel;

        /* Put all controls at a sane state */
        for (i = 0; i < ARRAY_SIZE(s2255_qctrl); i++) {
                qctl_regs[i] =s2255_qctrl[i].default_value;
        }

        dprintk(1,"Open: fh=0x%08lx, dev=0x%08lx, dev->vidq=0x%08lx\n",
                (unsigned long)fh,(unsigned long)dev,(unsigned long)&dev->vidq[cur_channel]);
        dprintk(1,"Open: list_empty queued=%d\n",list_empty(&dev->vidq[cur_channel].queued));
        dprintk(1,"Open: list_empty active=%d\n",list_empty(&dev->vidq[cur_channel].active));
        dprintk(1, "s2255core_board_open\n");

        videobuf_queue_vmalloc_init(&fh->vb_vidq, &s2255_video_qops,
                                    NULL, NULL,
                                    fh->type,
                                    V4L2_FIELD_INTERLACED,
                                    sizeof(struct s2255_buffer),fh);

        kref_get(&dev->kref);
        mutex_unlock( &usb_s2255_open_mutex);
        dprintk(2, "v4l open done\n");
        return 0;
}

static ssize_t
s2255_read(struct file *file, char __user *data, size_t count, loff_t *ppos)
{
        struct s2255_fh        *fh = file->private_data;

        if (fh->type==V4L2_BUF_TYPE_VIDEO_CAPTURE) {
                if (res_locked(fh->dev,fh))
                        return -EBUSY;
                return videobuf_read_one(&fh->vb_vidq, data, count, ppos,
                                         file->f_flags & O_NONBLOCK);
        }
        return 0;
}

static unsigned int
s2255_poll(struct file *file, struct poll_table_struct *wait)
{
        struct s2255_fh        *fh = file->private_data;
        struct s2255_buffer    *buf;

        dprintk(100,"%s\n",__FUNCTION__);

        if (V4L2_BUF_TYPE_VIDEO_CAPTURE != fh->type)
                return POLLERR;

        if (res_get(fh->dev,fh)) {
                dprintk(100,"poll: mmap interface\n");
                /* streaming capture */
                if (list_empty(&fh->vb_vidq.stream))
                        return POLLERR;
                buf = list_entry(fh->vb_vidq.stream.next,struct s2255_buffer,
                                 vb.stream);
        } else {
                dprintk(100,"poll: read() interface\n");
                /* read() capture */
                buf = (struct s2255_buffer*)fh->vb_vidq.read_buf;
                if (NULL == buf)
                        return POLLERR;
        }
        poll_wait(file, &buf->vb.done, wait);
        if (buf->vb.state == VIDEOBUF_DONE ||
            buf->vb.state == VIDEOBUF_ERROR)
                return POLLIN|POLLRDNORM;
        return 0;
}

static int s2255_release_v4l(struct inode *inode, struct file *file)
{
        struct s2255_fh         *fh = file->private_data;
        struct s2255_dev *dev       = fh->dev;
        struct s2255_dmaqueue *vidq = &dev->vidq[fh->channel];
        int minor = iminor(inode);


        dev->users[fh->channel]--;

        s2255_stop_acquire( dev, fh->channel);
        s2255_stop_thread(vidq);

        videobuf_mmap_free(&fh->vb_vidq);
        kfree (fh);
        kref_put(&dev->kref, s2255_destroy);
        printk(KERN_DEBUG "s2255: close called (minor=%d, users=%d)\n",minor,dev->users[fh->channel]);

        return 0;
}

static int
s2255_mmap_v4l(struct file *file, struct vm_area_struct * vma)
{
        struct s2255_fh        *fh = file->private_data;
        int ret;

        dprintk (4,"mmap called, vma=0x%08lx\n",(unsigned long)vma);

        ret=videobuf_mmap_mapper(&fh->vb_vidq, vma);

        dprintk (4,"vma start=0x%08lx, size=%ld, ret=%d\n",
                 (unsigned long)vma->vm_start,
                 (unsigned long)vma->vm_end-(unsigned long)vma->vm_start,
                 ret);

        return ret;
}

static const struct file_operations s2255_fops_v4l = {
        .owner          = THIS_MODULE,
        .open           = s2255_open_v4l,
        .release        = s2255_release_v4l,
        .read           = s2255_read,
        .poll           = s2255_poll,
        .ioctl          = video_ioctl2, /* V4L2 ioctl handler */
        .mmap           = s2255_mmap_v4l,
        .llseek         = no_llseek,
};

static struct video_device template = {
        .name           = "s2255v",
        .type           = VID_TYPE_CAPTURE,
        .fops           = &s2255_fops_v4l,
        .minor          = -1,
        .vidioc_querycap      = vidioc_querycap,
        .vidioc_enum_fmt_cap  = vidioc_enum_fmt_cap,
        .vidioc_g_fmt_cap     = vidioc_g_fmt_cap,
        .vidioc_try_fmt_cap   = vidioc_try_fmt_cap,
        .vidioc_s_fmt_cap     = vidioc_s_fmt_cap,
        .vidioc_reqbufs       = vidioc_reqbufs,
        .vidioc_querybuf      = vidioc_querybuf,
        .vidioc_qbuf          = vidioc_qbuf,
        .vidioc_dqbuf         = vidioc_dqbuf,
        .vidioc_s_std         = vidioc_s_std,
        .vidioc_enum_input    = vidioc_enum_input,
        .vidioc_g_input       = vidioc_g_input,
        .vidioc_s_input       = vidioc_s_input,
        .vidioc_queryctrl     = vidioc_queryctrl,
        .vidioc_g_ctrl        = vidioc_g_ctrl,
        .vidioc_s_ctrl        = vidioc_s_ctrl,
        .vidioc_streamon      = vidioc_streamon,
        .vidioc_streamoff     = vidioc_streamoff,
#ifdef CONFIG_VIDEO_V4L1_COMPAT
        .vidiocgmbuf          = vidiocgmbuf,
#endif
        .tvnorms              = S2255_NORMS,
        .current_norm         = V4L2_STD_NTSC_M,
};



static int s2255_probe_v4l(struct s2255_dev *dev)
{
        int ret;
        int i;
        int cur_nr = video_nr;
        /* initialize all video 4 linux */
        list_add_tail(&dev->s2255_devlist, &s2255_devlist);
        /* register 4 video devices */
        for (i=0; i<MAX_CHANNELS;i++) {
                INIT_LIST_HEAD(&dev->vidq[i].active);
                INIT_LIST_HEAD(&dev->vidq[i].queued);
                init_waitqueue_head(&dev->vidq[i].wq);
                dev->vidq[i].timeout.function = s2255_vid_timeout;
                dev->vidq[i].timeout.data     = (unsigned long)&dev->vidq[i];
                dev->vidq[i].dev = dev;
                dev->vidq[i].channel = i;
                dev->vidq[i].kthread = NULL;
                init_timer(&dev->vidq[i].timeout);
                /* register 4 video devices */
                dev->vdev[i] = video_device_alloc();
                memcpy( dev->vdev[i], &template,sizeof(struct video_device));
                if(video_nr == -1) {
                        ret = video_register_device(dev->vdev[i], VFL_TYPE_GRABBER, video_nr);
                } else {
                        ret = video_register_device(dev->vdev[i], VFL_TYPE_GRABBER, cur_nr+i);
                }
                dev->vdev[i]->priv = dev;

                if( ret != 0) {
                        printk("failed register video device!\n");
                        return ret;
                }
        }
        printk(KERN_INFO "Sensoray 2255 V4L driver\n");
        return ret;
}

static void s2255_exit_v4l(struct s2255_dev *dev)
{
        struct list_head *list;
        int i;
        /* unregister the video devices */
        while (!list_empty(&s2255_devlist)) {
                list = s2255_devlist.next;
                list_del(list);
        }

        for (i=0;i<MAX_CHANNELS;i++) {
                video_unregister_device(dev->vdev[i]);
                del_timer(&dev->vidq[i].timeout);
        }
}


#define EP_NUM_CONFIG 2
/* write to the configuration pipe, synchronously */
int s2255_write_config( struct usb_device *udev, unsigned char *pbuf, int size)
{
        int pipe;
        int done;
        long retval = -1;
        if(udev) {
                pipe = usb_sndbulkpipe(udev,EP_NUM_CONFIG);
                retval = usb_bulk_msg( udev, pipe, pbuf, size, &done, 500);
        }
        return retval;
}

/* this function moves the usb stream read pipe data
 * into the system buffers.
 *  returns 0 on success, EAGAIN if more data to process( call this
 *  function again).
 *
 *  Received frame structure:
 *  bytes 0-3:  marker : 0x2255DA4AL (FRAME_MARKER)
 *  bytes 4-7:  channel: 0-3
 *  bytes 8-11: payload size:  size of the frame
 *  bytes 12-payloadsize+12:  frame data
 */
static int save_frame( struct s2255_dev *dev, struct s2255_pipeinfo *pPipeInfo)
{
        char *pdest;
        u32 offset = 0;
        int bsync = 0;
        int btrunc = 0;
        static int dbgsync = 0;
        char *psrc;
        unsigned long copy_size;
        unsigned long size;
        s32 idx = -1;
        struct framei *frm;
        unsigned char *pdata;
        unsigned long cur_size;
        int bsearch = 0;
        struct bufferi *buf;
        dprintk(100,"buffer to user\n");

        idx = dev->cur_frame[dev->cc];
        buf = &dev->buffer[dev->cc];
        frm = &buf->frame[idx];

        if( frm->ulState == 0) {
                frm->ulState = 1;
                frm->cur_size = 0;
                bsearch = 1;
        }
        else if( frm->ulState == 2) {
                /* system frame was not freed */
                dprintk(2,"sys frame not free.  overrun ringbuf\n");
                bsearch = 1;
                frm->ulState = 1;
                frm->cur_size = 0;
        }

        if( bsearch ) {
                if( *(s32 *)pPipeInfo->pTransferBuffer != FRAME_MARKER ) {
                        u32 jj;
                        if( dbgsync == 0) {
                                dprintk(3,"not synched, discarding all packets"
                                        "until marker\n");

                                dbgsync++;
                        }
                        pdata = (unsigned char *) pPipeInfo->pTransferBuffer;
                        for( jj=0;jj<(pPipeInfo->cur_transfer_size-12); jj++) {
                                if( *(s32 *)pdata == FRAME_MARKER ) {
                                        int cc;
                                        dprintk(3,"found frame marker at offset:"
                                                " %d [%x %x]\n", jj, pdata[0],
                                                pdata[1]);
                                        offset = jj;
                                        bsync = 1;
                                        cc = *(u32 *)(pdata + sizeof( u32));
                                        if( cc >= MAX_CHANNELS) {
                                                printk( KERN_ERR "bad channel\n");
                                                return -EINVAL;
                                        }
                                        /* reverse it */
                                        dev->cc = G_chnmap[cc];
                                        break;
                                }
                                pdata++;
                        }
                        if( bsync == 0) {
                                return -EINVAL;
                        }
                } else {
                        u32 *pword;
                        u32 payload;
                        int cc;
                        dbgsync = 0;
                        bsync = 1;
                        pword = (u32 *)pPipeInfo->pTransferBuffer;
                        cc = pword[1];

                        if( cc >= MAX_CHANNELS) {
                                printk("invalid channel found.  throwing out data!\n");
                                return -EINVAL;
                        }
                        dev->cc = G_chnmap[cc];
                        payload = pword[2];
                        if( payload != dev->req_image_size[dev->cc]) {
                                dprintk(1,"[%d][%d]unexpected payload: %d"
                                        "required: %lu \n", cc, dev->cc,
                                        payload, dev->req_image_size[dev->cc]);
                                dev->bad_payload[dev->cc]++;
                                /* discard the bad frame */
                                return -EINVAL;
                        }

                }
        }
        /* search done.  now find out if should be acquiring
           on this channel */
        if( !dev->b_acquire[dev->cc]) {
                return -EINVAL;
        }

        idx = dev->cur_frame[dev->cc];
        frm = &dev->buffer[dev->cc].frame[idx];

        if( frm->ulState == 0) {
                frm->ulState = 1;
                frm->cur_size = 0;
        }
        else if( frm->ulState == 2) {
                /* system frame ring buffer overrun */
                dprintk(2,"sys frame overrun.  overwriting frame %d %d\n", dev->cc, idx);
                frm->ulState = 1;
                frm->cur_size = 0;
        }

        if( bsync) {
                /* skip the marker 512 bytes (and offset if out of sync) */
                psrc = (char *)pPipeInfo->pTransferBuffer + offset + PREFIX_SIZE;
        }
        else {
                psrc = (char *)pPipeInfo->pTransferBuffer;
        }

        if( frm->lpvbits == NULL) {
                dprintk(1,"s2255 frame buffer == NULL.%p %p %d %d",
                        frm,dev,dev->cc,idx);
                return -ENOMEM;
        }

        pdest = frm->lpvbits + frm->cur_size;

        if( bsync) {
                copy_size = (pPipeInfo->cur_transfer_size - offset) - PREFIX_SIZE;
                if( copy_size > pPipeInfo->cur_transfer_size) {
                        printk("invalid copy size, overflow!\n");
                        return -ENOMEM;
                }
        }
        else {
                copy_size = pPipeInfo->cur_transfer_size;
        }

        cur_size = frm->cur_size;
        size = dev->req_image_size[dev->cc];

        if( (copy_size + cur_size) > size) {
                copy_size = size - cur_size;
                btrunc = 1;
        }

        memcpy(pdest, psrc, copy_size);
        cur_size += copy_size;
        frm->cur_size += copy_size;
        dprintk( 50,"cur_size size %lu size %lu \n", cur_size, size);

        if ( cur_size >= ( size - PREFIX_SIZE) ) {
                u32 cc = dev->cc;
                frm->ulState = 2;
                dprintk(2,"****************[%d]Buffer[%d]full*************\n"
                        ,cc,idx);
                dev->last_frame[cc] = dev->cur_frame[cc];
                dev->cur_frame[cc]++;
                /* end of system frame ring buffer, start at zero */
                if( (dev->cur_frame[cc] == SYS_FRAMES) || (dev->cur_frame[cc] == dev->buffer[cc].dwFrames) ) {
                        dev->cur_frame[cc] = 0;
                }
                /* signal the semaphore for this channel */
                s2255_got_frame( dev, cc);
                dev->frame_count[cc]++;
        }
        /* frame was truncated */
        if( btrunc) {
                /* return more data to process */
                return EAGAIN;
        }
        /* done successfully */
        return 0;
}



void s2255_read_video_callback(struct s2255_dev *dev,struct s2255_pipeinfo *pPipeInfo)
{
        int res;
        int b_acq = 0;
        int j;
        dprintk(50,"callback read video \n");

        if( dev->cc >= MAX_CHANNELS) {
                dev->cc = 0;
                printk("invalid channel\n");
                return;
        }

        for( j=0;j<MAX_CHANNELS;j++) {
                if( dev->b_acquire[j] ) {
                        b_acq = 1;
                        break;
                }
        }
        /* if not acquiring on any channel, just return and complete
           the urb callback function */
        if( !b_acq) {
                return;
        }

        /* otherwise copy to the system buffers */
        res = save_frame( dev, pPipeInfo);
        if( res == EAGAIN) {
                save_frame( dev,pPipeInfo);

        }
        dprintk(50,"callback read video done\n");
        return;
}


static int s2255_board_init( struct s2255_dev *dev)
{
        int j;
        struct mode2255i mode_def = { DEF_MODEI_NTSC_CONT };
        int fw_ver;
        dprintk(4, "board init: %p", dev);

        for( j=0;j<MAX_CHANNELS;j++) {
                dev->b_acquire[j] = 0;
                dev->mode[j] = mode_def;
                dev->req_image_size[j] = get_transfer_size( &mode_def);
        }

        for( j=0;j<MAX_PIPE_BUFFERS;j++) {
                struct s2255_pipeinfo *pPipeInfo = &dev->UsbPipes[j];
                memset(pPipeInfo, sizeof(struct s2255_pipeinfo), 0);
                pPipeInfo->state = 0;
                pPipeInfo->prev_state = 0;
                pPipeInfo->dev = dev;
                pPipeInfo->cur_transfer_size = DEFAULT_PIPE_USBBLOCK;
                pPipeInfo->max_transfer_size = MAX_PIPE_USBBLOCK;

                if( pPipeInfo->cur_transfer_size > pPipeInfo->max_transfer_size) {
                        pPipeInfo->cur_transfer_size = pPipeInfo->max_transfer_size;
                }
                pPipeInfo->pTransferBuffer = (unsigned char *) kzalloc( pPipeInfo->max_transfer_size, GFP_KERNEL );
                if( pPipeInfo->pTransferBuffer == NULL) {
                        dprintk(1,"out of memory!\n");
                        return -ENOMEM;
                }


        }

        /* query the firmware */
        fw_ver = s2255_get_fx2fw(dev);

        printk( KERN_INFO "2255 usb firmware version %d \n", fw_ver);
        if( fw_ver < CUR_USB_FWVER) {
                err("usb firmware not up to date %d\n", fw_ver);
        }



        for( j=0;j<MAX_CHANNELS;j++) {
                dev->b_acquire[j] = 0;
                dev->mode[j] = mode_def;
                dev->req_image_size[j] = get_transfer_size( &mode_def);
                dev->frame_count[j] = 0;
                /* create the system buffers */
                s2255_create_sys_buffers(dev, j);
        }
        /* start read pipe */
        s2255_start_readpipe(dev);

        dprintk(1,"S2255: board initialized\n");
        return 0;
}



/* Create the system ring buffer to copy frames into from the
 * usb read pipe.
 */
static int s2255_create_sys_buffers( struct s2255_dev *dev,unsigned long chn)
{
        unsigned long i;
        unsigned long reqsize;
        dprintk(1,"create sys buffers\n");
        if( chn >= MAX_CHANNELS) {
                return -1;
        }
        dev->buffer[chn].dwFrames = SYS_FRAMES;

        /* always allocate maximum size(PAL) for system buffers */
        reqsize = SYS_FRAMES_MAXSIZE;

        if (reqsize > SYS_FRAMES_MAXSIZE) {
                reqsize = SYS_FRAMES_MAXSIZE;
        }


        for (i=0;i< SYS_FRAMES;i++) {
                /* allocate the frames */
                dev->buffer[chn].frame[i].lpvbits = vmalloc( reqsize);

                dprintk(1,"valloc %p chan %lu, idx %lu, pdata %p\n",
                        &dev->buffer[chn].frame[i],chn, i,
                        dev->buffer[chn].frame[i].lpvbits );
                dev->buffer[chn].frame[i].size = reqsize;
                if( dev->buffer[chn].frame[i].lpvbits == NULL) {
                        printk( KERN_INFO "out of memory.  using less frames\n");
                        dev->buffer[chn].dwFrames = i;
                        break;
                }
        }

        /* make sure internal states are set */
        for (i=0;i<SYS_FRAMES;i++) {
                dev->buffer[chn].frame[i].ulState = 0;
                dev->buffer[chn].frame[i].cur_size = 0;
        }

        dev->cur_frame[chn] = 0;
        dev->last_frame[chn] = -1;
        return 0;
}


static int s2255_release_sys_buffers( struct s2255_dev *dev, unsigned long channel )
{
        unsigned long i;
        dprintk(1,"release sys buffers\n");
        for (i= 0;i< SYS_FRAMES;i++) {
                if (dev->buffer[channel].frame[i].lpvbits) {
                        dprintk(1,"vfree %p\n",
                                dev->buffer[channel].frame[i].lpvbits );
                        vfree(dev->buffer[channel].frame[i].lpvbits);
                }
                dev->buffer[channel].frame[i].lpvbits = NULL;
        }
        return 0;
}





static int s2255_board_shutdown( struct s2255_dev *dev)
{
        u32 i;

        dprintk(1,"S2255: board close: %p", dev);

        for( i=0;i<MAX_CHANNELS;i++) {
                s2255_stop_acquire( dev, i);
        }
        s2255_stop_readpipe(dev);

        for(i=0;i<MAX_CHANNELS;i++) {
                s2255_release_sys_buffers( dev, i);
        }
        /* release transfer buffers */
        for (i=0;i<MAX_PIPE_BUFFERS;i++) {
                struct s2255_pipeinfo *pPipeInfo = &dev->UsbPipes[i];
                if (pPipeInfo->pTransferBuffer) {
                        kfree(pPipeInfo->pTransferBuffer);
                }
        }
        return 0;
}



static void read_pipe_completion(struct urb *purb)
{
        struct s2255_pipeinfo *pPipeInfo;
        struct s2255_dev *dev;
        int status;
        int pipe;

        pPipeInfo = (struct s2255_pipeinfo*) purb->context;
        dprintk(100, "read pipe completion %p, status %d\n", purb, purb->status);
        if (pPipeInfo == NULL) {
                printk("no context !\n");
                return;
        }

        dev = (struct s2255_dev *) pPipeInfo->dev;

        if (dev == NULL) {
                printk("no context !\n");
                return;
        }
        status = purb->status;
        if (status != 0) {
                dprintk(2,"read_pipe_completion: err\n");
                return;
        }

        if (pPipeInfo->state == 0) {
                dprintk(2,"exiting USB pipe");
                return;
        }

        s2255_read_video_callback(dev, pPipeInfo);

        pPipeInfo->err_count = 0;
        pipe = usb_rcvbulkpipe(dev->udev,dev->read_endpoint);
        /* reuse urb */
        usb_fill_bulk_urb(pPipeInfo->pStreamUrb, dev->udev,
                          pipe,
                          pPipeInfo->pTransferBuffer,
                          pPipeInfo->cur_transfer_size,
                          read_pipe_completion, pPipeInfo);

        if( pPipeInfo->state != 0 ) {
                if( usb_submit_urb( pPipeInfo->pStreamUrb, GFP_KERNEL)) {
                        printk("error submitting urb\n");
                        usb_free_urb(pPipeInfo->pStreamUrb);
                }
        }
        return;
}



int s2255_start_readpipe(struct s2255_dev *dev)
{
        int pipe;
        int retval;
        int i;
        struct s2255_pipeinfo *pPipeInfo = dev->UsbPipes;
        pipe = usb_rcvbulkpipe(dev->udev,dev->read_endpoint);
        dprintk(2,"start pipe IN %d\n", dev->read_endpoint);

        for (i=0;i<MAX_PIPE_BUFFERS;i++) {
                pPipeInfo->state = 1;
                pPipeInfo->buf_index = (u32) i;
                pPipeInfo->priority_set = 0;
                pPipeInfo->pStreamUrb = usb_alloc_urb(0,GFP_KERNEL);
                if (!pPipeInfo->pStreamUrb) {
                        printk("ReadStream : Unable to alloc URB");
                        return -ENOMEM;
                }
                /* transfer buffer allocated in board_init */
                usb_fill_bulk_urb(pPipeInfo->pStreamUrb, dev->udev,
                                  pipe,
                                  pPipeInfo->pTransferBuffer,
                                  pPipeInfo->cur_transfer_size,
                                  read_pipe_completion, pPipeInfo);

                pPipeInfo->urb_size = sizeof(pPipeInfo->pStreamUrb);
                dprintk(4,"submitting URB %p\n", pPipeInfo->pStreamUrb);
                retval = usb_submit_urb( pPipeInfo->pStreamUrb, GFP_KERNEL);
                if(retval) {
                        printk(KERN_ERR "s2255: start read pipe failed\n");
                        return retval;
                }
        }

        return 0;
}

void s2255_sleep( int ms)
{
        wait_queue_head_t sleep_q;
        DEFINE_WAIT(wait);
        if( ms == 0) {
                schedule();
                return;
        }
        init_waitqueue_head( &sleep_q);
        prepare_to_wait(&sleep_q,&wait, TASK_INTERRUPTIBLE);
        schedule_timeout((ms*HZ) / 1000);
        finish_wait( &sleep_q, &wait);
}

static void dump_verify_mode(struct mode2255i *mode)
{
    printk("-------------------------------------------------------\n");
    printk("verify mode\n");
    printk("format: %d\n", mode->format);
    printk("scale: %d\n", mode->scale);
    printk("fdec: %d\n", mode->fdec);
    printk("color: %d\n", mode->color);
    printk("bright: 0x%x\n", mode->bright);
    printk("restart: 0x%x\n", mode->restart);
    printk("Usbblock: 0x%x\n", mode->usb_block);
    printk("single: 0x%x\n", mode->single);
    printk("-------------------------------------------------------\n");
}

/* set mode is the function which controls the DSP.
 * the restart parameter in struct mode2255i should be set whenever
 * the image size could change via color format, video system or image
 * size.
 * When the restart parameter is set, we sleep for ONE frame to allow the
 * DSP time to get the new frame
 *
 */

static int s2255_set_mode(struct s2255_dev *dev, unsigned long chn,
                          struct mode2255i *mode)
{
        int res;
        u32 *pBuf;
        unsigned long chn_rev;

        chn_rev = G_chnmap[chn];
        dprintk(3,"mode scale [%ld] %p %d\n", chn, mode, mode->scale);
        dprintk(3,"mode scale [%ld] %p %d\n", chn, &dev->mode[chn],
                dev->mode[chn].scale);
        dprintk(2,"mode contrast %x\n", mode->contrast);

        /* save the mode */
        dev->mode[chn] = *mode;
        dev->req_image_size[chn] = get_transfer_size( mode);
        dprintk(1, "transfer size %ld\n", dev->req_image_size[chn]);

        pBuf = (u32 *) kzalloc( 512, GFP_KERNEL);
        if( pBuf == NULL) {
                printk("out of mem\n");
                return -1;
        }

        /* set the mode */
        pBuf[0] = IN_DATA_TOKEN;
        pBuf[1] = (u32) chn_rev;
        pBuf[2] = CMD_SET_MODE;
        memcpy( &pBuf[3], &dev->mode[chn], sizeof( struct mode2255i));
        res = s2255_write_config( dev->udev, (unsigned char *) pBuf,512);
        if( debug) {
                dump_verify_mode( mode);
        }
        kfree(pBuf);
        dprintk(1,"set mode done chn %lu, %d\n", chn, res);

        /* wait at least one frame before continuing */

        s2255_sleep(40);

        /* clear the restart flag */
        dev->mode[chn].restart = 0;

        return res;
}


/** starts acquisition process
 *
 */
static int s2255_start_acquire( struct s2255_dev *dev, unsigned long chn)
{
        unsigned char *pBuf;
        int res;
        unsigned long chn_rev;

        if( chn >= MAX_CHANNELS) {
                dprintk(2, "start acquire failed, bad channel %lu\n", chn);
                return -1;
        }
        chn_rev = G_chnmap[chn];
        dprintk(1, "S2255: start acquire %lu \n", chn);

        pBuf = (unsigned char *)kzalloc(512,GFP_KERNEL);
        if( pBuf == NULL) {
                printk("out of mem\n");
                return -1;
        }
        /* send the start command */
        *(u32 *)pBuf = IN_DATA_TOKEN;
        *((u32 *) pBuf + 1) = (u32) chn_rev;
        *((u32 *) pBuf + 2) = (u32) CMD_START;
        res = s2255_write_config( dev->udev, (unsigned char *)pBuf, 512);
        if( res != 0 ) {
                printk("S2255: CMD_START error\n");
        }

        dprintk(2, "start acquire exit[%lu] %d \n", chn,res);
        kfree( pBuf);
        return 0;
}

static int s2255_stop_acquire( struct s2255_dev *dev, unsigned long chn)
{
        unsigned char *pBuf;
        int res;
        unsigned long chn_rev;

        if( chn >= MAX_CHANNELS) {
                dprintk(2, "stop acquire failed, bad channel %lu\n", chn);
                return -1;
        }
        chn_rev = G_chnmap[chn];

        pBuf = (unsigned char *)kzalloc(512,GFP_KERNEL);
        if( pBuf == NULL) {
                printk("out of mem\n");
                return -1;
        }

        /* send the stop command */
        dprintk(1, "stop acquire %lu\n", chn );
        *(u32 *)pBuf = IN_DATA_TOKEN;
        *((u32 *) pBuf + 1) = (u32) chn_rev;
        *((u32 *) pBuf + 2) = CMD_STOP;
        res = s2255_write_config( dev->udev, (unsigned char *)pBuf, 512);

        if( res!=0 ) {
                printk("CMD_STOP error\n");
        }

        dprintk(4, "stop acquire: releasing states \n");


        kfree(pBuf);


        return 0;
}


static void s2255_stop_readpipe(struct s2255_dev *dev)
{
        int j;
        if (dev == NULL) {
                printk("s2255: invalid device\n");
                return;
        }
        dprintk(4, "stop read pipe\n");
        for (j=0;j<MAX_PIPE_BUFFERS;j++) {
                struct s2255_pipeinfo *pPipeInfo = &dev->UsbPipes[j];
                if (pPipeInfo) {
                        if (pPipeInfo->state == 0) {
                                continue;
                        }
                        pPipeInfo->state = 0;
                        pPipeInfo->prev_state = 1;

                }
        }

        for (j=0;j<MAX_PIPE_BUFFERS;j++) {
                struct s2255_pipeinfo *pPipeInfo = &dev->UsbPipes[j];
                if (pPipeInfo->pStreamUrb) {
                        /* cancel urb */
                        usb_kill_urb( pPipeInfo->pStreamUrb);
                        usb_free_urb(pPipeInfo->pStreamUrb);
                        pPipeInfo->pStreamUrb = NULL;
                }
        }
        dprintk(2,"s2255 stop read pipe: %d\n",j);
        return;
}


static long s2255_vendor_req(struct s2255_dev *dev,
                      unsigned char Request, u16 Index,
                      u16 Value,void *TransferBuffer,
                      s32 TransferBufferLength, int bOut)
{
        int r;
        if(!bOut) {
                r = usb_control_msg(dev->udev, usb_rcvctrlpipe(dev->udev, 0),
                                    Request,
                                    USB_TYPE_VENDOR | USB_RECIP_DEVICE |
                                    USB_DIR_IN,
                                    Value, Index, TransferBuffer,
                                    TransferBufferLength, HZ * 5);
        }
        else {
                r = usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0),
                                    Request, USB_TYPE_VENDOR | USB_RECIP_DEVICE,
                                    Value, Index, TransferBuffer,
                                    TransferBufferLength, HZ * 5);
        }
        return r;
}




static u32 get_transfer_size( struct mode2255i *mode)
{
        int                     linesPerFrame = LINE_SZ_DEF;
        int                     pixelsPerLine = NUM_LINES_DEF;
        u32      outImageSize;
        u32      usbInSize;
        unsigned int  mask_mult;

        if( mode == 0) {
                return 0;
        }

        if (mode->format == FORMAT_NTSC) {
                switch (mode->scale) {
                case SCALE_4CIFS:
                        linesPerFrame = NUM_LINES_4CIFS_NTSC * 2;
                        pixelsPerLine = LINE_SZ_4CIFS_NTSC;
                        break;
                case SCALE_2CIFS:
                        linesPerFrame = NUM_LINES_2CIFS_NTSC;
                        pixelsPerLine = LINE_SZ_2CIFS_NTSC;
                        break;
                case SCALE_1CIFS:
                        linesPerFrame = NUM_LINES_1CIFS_NTSC;
                        pixelsPerLine = LINE_SZ_1CIFS_NTSC;
                        break;
                default:
                        break;
                }
        } else if (mode->format == FORMAT_PAL) {
                switch (mode->scale) {
                case SCALE_4CIFS:
                        linesPerFrame = NUM_LINES_4CIFS_PAL * 2;
                        pixelsPerLine = LINE_SZ_4CIFS_PAL;
                        break;
                case SCALE_2CIFS:
                        linesPerFrame = NUM_LINES_2CIFS_PAL;
                        pixelsPerLine = LINE_SZ_2CIFS_PAL;
                        break;
                case SCALE_1CIFS:
                        linesPerFrame = NUM_LINES_1CIFS_PAL;
                        pixelsPerLine = LINE_SZ_1CIFS_PAL;
                        break;
                default:
                        break;
                }
        }
        outImageSize = linesPerFrame * pixelsPerLine;
        if (mode->color != COLOR_Y8) {
                /* 2 bytes/pixel if not monochrome */
                outImageSize *= 2;
        }

        /* total bytes to send including prefix and 4K padding;
           must be a multiple of USB_READ_SIZE */
        usbInSize = outImageSize + PREFIX_SIZE; /* always send prefix */
        mask_mult = 0xffffffffUL - DEF_USB_BLOCK + 1 ;
        /* if size not a multiple of USB_READ_SIZE */
        if (usbInSize & ~mask_mult) {
                usbInSize = (usbInSize & mask_mult) + (DEF_USB_BLOCK);
        }
        return usbInSize;
}

/** convert from YUV(YCrCb) to RGB
 * 65536 R = 76533(Y-16) + 104936 * (Cr-128)
 * 65536 G = 76533(Y-16) - 53451(Cr-128) - 25703(Cb -128)
 * 65536 B = 76533(Y-16) + 132677(Cb-128)
 */
static void YCrCb2RGB
( int Y, int Cr, int Cb, unsigned char *pR, unsigned char *pG, unsigned char *pB)
{
    int R,G,B;

    Y = Y - 16;
    Cr = Cr - 128;
    Cb = Cb - 128;

    R = (76533 * Y + 104936 * Cr) >> 16;
    G = ((76533 * Y) - (53451 * Cr) - (25703 * Cb)) >> 16;
    B = ((76533 * Y) + (132677 * Cb)) >> 16;
    /* even with proper conversion, some values still need clipping. */
    if (R > 255) {
            R = 255;
    }
    if (G > 255) {
            G = 255;
    }
    if (B > 255) {
            B = 255;
    }
    if (R < 0) {
            R = 0;
    }
    if (G < 0) {
            G = 0;
    }
    if (B < 0) {
            B = 0;
    }
    *pR = R;
    *pG = G;
    *pB = B;
    return;
}

/** converts 2255 planar format to yuyv
 */
static void planar422p_to_yuy2(const unsigned char *in, unsigned char *out,
                               int width, int height)
{
        unsigned char *pY;
        unsigned char *pCb;
        unsigned char *pCr;
        unsigned long  size = height * width;
        unsigned int i;
        pY = (unsigned char *) in;
        pCr =(unsigned char *) in + height * width;
        pCb = (unsigned char *) in + height * width +  (height * width /2);
        for( i=0;i<size*2;i+=4) {
                out[i] = *pY++;
                out[i+1] = *pCr++;
                out[i+2] = *pY++;
                out[i+3] = *pCb++;
        }
        return;
}

/** basic 422 planar to RGB24 or BGR24 software conversion.
 *  This is best done with MMX. Update to kernel function
 *  when image conversion functions added to kernel.
 *
 */
static void planar422p_to_rgb24(const unsigned char *in,
                                unsigned char *out, int width,
                                int height, int rev_order)
{
    unsigned char *pY;
    unsigned char *pYEND;
    unsigned char *pCb;
    unsigned char *pCr;
    unsigned char Cr,Cb,Y,r,g,b;
    unsigned long k = 0;
    pY = (unsigned char *) in;
    pCb =(unsigned char *) in + (height * width);
    pCr = (unsigned char *) in + (height * width) +  (height * width /2);
    pYEND = pCb;
    while( pY<pYEND ) {
        Y = *pY++;
        Cr = *pCr;
        Cb = *pCb;
        YCrCb2RGB( Y,Cr,Cb,&r,&g,&b);
        out[k++] = !rev_order ? b : r;
        out[k++] = g;
        out[k++] = !rev_order ? r : b ;
        if( pY >= pYEND) {
            break;
        }
        Y = *pY++;
        Cr = *pCr++;
        Cb = *pCb++;
        YCrCb2RGB( Y,Cr,Cb,&r,&g,&b);
        out[k++] = !rev_order ? b : r;
        out[k++] = g;
        out[k++] = !rev_order ? r : b ;
    }
    return;
}

static void planar422p_to_rgb32(  const unsigned char *in, unsigned char *out, int width, int height, int rev_order)
{
    unsigned char *pY;
    unsigned char *pYEND;
    unsigned char *pCb;
    unsigned char *pCr;
    unsigned char Cr,Cb,Y,r,g,b;
    unsigned long k = 0;
    pY = (unsigned char *) in;
    pCb =(unsigned char *) in + (height * width);
    pCr = (unsigned char *) in + (height * width) +  (height * width /2);
    pYEND = pCb;
    while (pY<pYEND ) {
        Y = *pY++;
        Cr = *pCr;
        Cb = *pCb;
        YCrCb2RGB( Y,Cr,Cb,&r,&g,&b);
        out[k++] = rev_order ? b : r;
        out[k++] = g;
        out[k++] = rev_order ? r : b;
        out[k++] = 0;
        if( pY >= pYEND) {
            break;
        }
        Y = *pY++;
        Cr = *pCr++;
        Cb = *pCb++;
        YCrCb2RGB( Y,Cr,Cb,&r,&g,&b);
        out[k++] = rev_order ? b : r;
        out[k++] = g;
        out[k++] = rev_order ? r : b;
        out[k++] = 0;
    }

    return;
}


static void planar422p_to_rgb565(  unsigned char const *in, unsigned char *out, int width, int height, int rev_order)
{
    unsigned char *pY;
    unsigned char *pYEND;
    unsigned char *pCb;
    unsigned char *pCr;
    unsigned char Cr,Cb,Y,r,g,b;
    unsigned long k = 0;
    unsigned short rgbbytes;
    pY = (unsigned char *) in;
    pCb =(unsigned char *) in + (height * width);
    pCr = (unsigned char *) in + (height * width) +  (height * width /2);
    pYEND = pCb;
    while( pY<pYEND ) {
        Y = *pY++;
        Cr = *pCr;
        Cb = *pCb;
        YCrCb2RGB(Y, Cr, Cb, &r, &g, &b);
        r = r >> 3;
        g = g >> 2;
        b = b >> 3;
        if( rev_order) {
                rgbbytes = b + (g<<5) + (r<<(5+6));
        } else {
                rgbbytes = r + (g<<5) + (b<<(5+6));
        }
        out[k++] = rgbbytes & 0xff;
        out[k++] = (rgbbytes >> 8) & 0xff;
        Y = *pY++;
        Cr = *pCr++;
        Cb = *pCb++;
        YCrCb2RGB( Y,Cr,Cb,&r,&g,&b);
        r = r >> 3;
        g = g >> 2;
        b = b >> 3;
        if( rev_order) {
                rgbbytes = b + (g<<5) + (r<<(5+6));
        } else {
                rgbbytes = r + (g<<5) + (b<<(5+6));
        }
        out[k++] = rgbbytes & 0xff;
        out[k++] = (rgbbytes >> 8) & 0xff;
    }
    return;
}


/** retrieve FX2 firmware version. future use.
 * @param dev pointer to device extension
 * @return -1 for fail, else returns firmware version as an int(16 bits)
 */
static int s2255_get_fx2fw( struct s2255_dev *dev)
{
        int fw;
        int ret;
        unsigned char transBuffer[64];
        ret = s2255_vendor_req(dev, VX_FW, 0, 0, transBuffer, 2, DIR_IN);
        if( ret<0) {
                dprintk(2,"get fw error: %x\n",ret);
        }
        fw = transBuffer[0] + (transBuffer[1] << 8) ;
        dprintk(2,"Get FW %x %x\n", transBuffer[0], transBuffer[1]);
        return fw;
}


module_init (usb_s2255_init);
module_exit (usb_s2255_exit);
MODULE_DESCRIPTION("Sensoray 2255 Video for Linux driver");
MODULE_AUTHOR("D.A.(Sensoray)");
MODULE_LICENSE("GPL");
