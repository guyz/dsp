#include <xdc/std.h>
#include <ti/sdo/ce/CERuntime.h>
#include <ti/sdo/ce/Engine.h>
#include <ti/sdo/ce/osal/Memory.h>
#include <ti/xdais/xdas.h>
#include <ti/xdais/dm/xdm.h>
#include <ti/sdo/ce/trace/gt.h>

#include <string.h> 
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include "frame.h"
#include "codec.h"
#include "util.h"
#include "memman.h"

#define INPUT_BUFFER_SIZE      1048576

static String engineName   = "ceEngine";
Engine_Handle           ce;

uint8_t *input_buf = NULL;

unsigned num_frames;
unsigned ce_frame_size;

Memory_AllocParams mem_params;

extern const struct codec h264_codec;
extern const struct codec mpeg4_codec;

struct frame *frames;

const struct codec *codecModule = NULL;

 int cont_alloc_frames(struct frame_format *ff, unsigned bufsize,
                  struct frame **fr, unsigned *nf) {
    int buf_w = ff->width, buf_h = ff->height;
    
    uint8_t *p = NULL;
    uint8_t *pp = NULL;
    unsigned y_offset;
    int i, clear_count;

    mem_params.type = Memory_CONTIGHEAP;
    mem_params.flags = Memory_NONCACHED;
    mem_params.align = 16; 

    input_buf = Memory_alloc(INPUT_BUFFER_SIZE, &mem_params);
    if (!input_buf) {
        fprintf(stderr, "Error allocating input buffer\n");
	return -1;  
    }

    ce_frame_size = buf_w * buf_h * 2;

    num_frames = bufsize / ce_frame_size;
    y_offset = buf_w * buf_h;

    fprintf(stderr, "MAIN CE Memory Manager: using %d CMEM HEAP allocated frame buffers, with size: %u\n", num_frames, ce_frame_size);

    frames = malloc(num_frames * sizeof(*frames));

    for (i = 0; i < num_frames; i++) {
	p = Memory_alloc(ce_frame_size, &mem_params);
    	if (!p) {
            fprintf(stderr, "Error allocating frame buffer %d on CMEM\n", i);
	    goto err;    
        }

	pp = (uint8_t *)Memory_getBufferPhysicalAddress(p, ce_frame_size, NULL);

        frames[i].virt[0] = p;
        frames[i].virt[1] = p + y_offset;
        frames[i].virt[2] = p + y_offset + buf_w / 2;
        frames[i].phys[0] = pp;
        frames[i].phys[1] = pp + y_offset;
        frames[i].phys[2] = pp + y_offset + buf_w / 2;
        frames[i].linesize[0] = ff->width;
        frames[i].linesize[1] = ff->width;
        frames[i].linesize[2] = ff->width;
    }

    ff->y_stride  = ff->width;
    ff->uv_stride = ff->width;

    *fr = frames;
    *nf = num_frames;

    return 0;

err:
    clear_count = i;
    for (i=0; i<clear_count; i++) {
	if (!frames) break;
	Memory_free(frames[i].virt[0], ce_frame_size, &mem_params);
    }

    Memory_free(input_buf, INPUT_BUFFER_SIZE, &mem_params);
    if (frames) free(frames);
    return -1;
}

 void
cont_free_frames(struct frame *frames, unsigned nf)
{
    int i;
    if (frames) {
       for (i=0; i<nf; i++) {
           if (frames[i].virt[0]) {
		//Memory_registerContigBuf((UInt32) frames[i].virt[0], ce_frame_size, (UInt32)frames[i].phys[0]);
	   	Memory_free(frames[i].virt[0], ce_frame_size, &mem_params);
	   }
       }
    }

    if (frames) free(frames); //TODO: reference this

    Memory_free(input_buf, INPUT_BUFFER_SIZE, &mem_params);

}


static int ce_open(const char *name, AVCodecContext *cc,
                    struct frame_format *ff)
{
    char decName[16];

    switch (cc->codec_id) {
	case CODEC_ID_H264:
	     codecModule = &h264_codec;
	     strcpy(decName, "h264dec");
	     break;
	case CODEC_ID_MPEG4:
	     codecModule = &mpeg4_codec;
	     strcpy(decName, "mpeg4dec");
	     break;
	default:
             fprintf(stderr, "ERROR: unsupported CE codec %d\n", cc->codec_id);
             return -1;
    }

    /* init Codec Engine */
    CERuntime_init();

    if ((ce = Engine_open(engineName, NULL, NULL)) == NULL) {
        fprintf(stderr, "ERROR: can't open engine ceEngine\n");
        return -1;
    }

    // Debug
    GT_set(Memory_GTNAME "+5");

    return codecModule->open(decName, cc, ff);
}



static int ce_decode(AVPacket *p)
{
    return codecModule->decode(p);
}


static void ce_close(void)
{
    codecModule->close();
    if (ce)         Engine_close(ce);               ce     = NULL;
    CERuntime_exit();
}


CODEC(avcodec) = {
    .name   = "ce",
    .flags  = OFBP_PHYS_MEM,
    .open   = ce_open,
    .decode = ce_decode,
    .close  = ce_close,
};

