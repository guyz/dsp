#include <xdc/std.h>

#include <ti/sdo/ce/video2/viddec2.h>
#include <ti/xdais/dm/ivideo.h>
#include <ti/xdais/dm/ividdec2.h>
#include <ti/xdais/xdas.h>
#include <ti/xdais/dm/xdm.h>
#include <ti/xdais/dm/ivideo.h>
#include <ti/xdais/dm/ividdec2.h>

#include <ti/sdo/ce/CERuntime.h>
#include <ti/sdo/ce/Engine.h>
#include <ti/sdo/ce/osal/Memory.h>

#include <string.h> 
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "im4h3dec.h"
#include "m4h3dec_ti.h"

#include <libavcodec/avcodec.h>
#include "frame.h"
#include "codec.h"
#include "util.h"

#define INPUT_BUFFER_SIZE      1048576

AVCodecContext           *avc;

extern Engine_Handle    ce;
VIDDEC2_Handle          dec;
String 			decoderName_mpeg4 = "mpeg4dec";

IVIDDEC2_Params          params;
IM4H3DEC_DynamicParams  dynamicParams;
IM4H3DEC_Status          status;
IM4H3DEC_InArgs          in_args;
IM4H3DEC_OutArgs         out_args;

XDAS_UInt8*  pOutputBuf [XDM_MAX_IO_BUFFERS];
XDAS_Int32   outBufSize [XDM_MAX_IO_BUFFERS];
XDAS_Int32   inBufSizes[5];
XDM1_BufDesc          *inbufs;
XDM_BufDesc           *outbufs;

extern uint8_t  *input_buf;
extern unsigned ce_frame_size;
extern unsigned num_frames;
extern struct frame *frames;
int firstTime = 1;

XDAS_Void setDynamicParams(IVIDDEC2_DynamicParams *dynamicParams)
{
    dynamicParams->decodeHeader  = XDM_DECODE_AU; // Supported
    dynamicParams->displayWidth  = 0;             // Supported : Default value is zero
    dynamicParams->frameSkipMode = IVIDEO_NO_SKIP;// Not Supported: Set to default value
    return;
}


static int mpeg4_open(const char *name, AVCodecContext *cc,
                    struct frame_format *ff)
{
    XDAS_Int32 err;

    if (cc->codec_id != CODEC_ID_MPEG4) {
        fprintf(stderr, "ERROR: unsupported CE codec %d\n", cc->codec_id);
        return -1;
    }

    ff->width  = ALIGN(cc->width, 16);
    ff->height = ALIGN(cc->height, 16);
    ff->disp_x = 0;
    ff->disp_y = 0;
    ff->disp_w = cc->width;
    ff->disp_h = cc->height;
    ff->pixfmt = PIX_FMT_YUYV422;

    /* Base class sizes */
    params.size                  		= sizeof(IVIDDEC2_Params);
    status.viddecStatus.size			= sizeof(IM4H3DEC_Status);
    dynamicParams.viddecDynamicParams.size	= sizeof(IM4H3DEC_DynamicParams);
    in_args.viddecInArgs.size			= sizeof(IM4H3DEC_InArgs);
    out_args.viddecOutArgs.size			= sizeof(IM4H3DEC_OutArgs);

	
    /* Init video decoder parameters */
    params.maxFrameRate    = 30;
    params.maxBitRate      = 10000000;
    params.dataEndianness  = XDM_BYTE;	
    params.maxHeight = 480;
    params.maxWidth = 864;
    params.forceChromaFormat = 4; // XDM_YUV_420P;

    /* allocate and initialize video decoder on the engine */
    dec = VIDDEC2_create(ce, decoderName_mpeg4, (IVIDDEC2_Params *)&params);
    if (dec == NULL) {
        fprintf(stderr, "ERROR: can't open codec %s\n", decoderName_mpeg4);
        goto err;
    }  

    /* Set the dynamic parameters */
    dynamicParams.postDeblock = 0;
    dynamicParams.postDering =  0; 
    dynamicParams.errorConceal = 0;
    dynamicParams.FrameLevelByteSwap = 1; 
    dynamicParams.viddecDynamicParams.mbDataFlag = 0;
    dynamicParams.viddecDynamicParams.frameOrder = IVIDDEC2_DISPLAY_ORDER; 

    setDynamicParams((IVIDDEC2_DynamicParams *)&dynamicParams);

    dynamicParams.useHighPrecIdctQp1 = 0;
    dynamicParams.viddecDynamicParams.displayWidth = 0;
    dynamicParams.viddecDynamicParams.newFrameFlag = 0;

    err = VIDDEC2_control(dec, XDM_RESET, (IVIDDEC2_DynamicParams *)&dynamicParams, (IVIDDEC2_Status *)&status);
    if (err) {
        fprintf(stderr, "VIDDEC2_control(XDM_RESET) failed %d\n", err);
        goto err;
    }

    err = VIDDEC2_control(dec, XDM_SETPARAMS, (IVIDDEC2_DynamicParams *)&dynamicParams, (IVIDDEC2_Status *)&status);
    if (err) {
        fprintf(stderr, "VIDDEC2_control(XDM_SETPARAMS) failed %d\n", err);
        goto err;
    }

    /* Init buffers and context information */
    err = VIDDEC2_control(dec, XDM_GETBUFINFO, (IVIDDEC2_DynamicParams *)&dynamicParams, (IVIDDEC2_Status *)&status);
    if (err) {
        fprintf(stderr, "VIDDEC2_control(XDM_GETBUFINFO) failed %d\n", err);
        goto err;
    }    

    inbufs = malloc(sizeof(*inbufs));
    outbufs = malloc(sizeof(*outbufs));

    inbufs->numBufs = 1;
    outbufs->numBufs = 1;
    inbufs->descs[0].bufSize = status.viddecStatus.bufInfo.minInBufSize[0];

    in_args.viddecInArgs.numBytes = 0;  
    out_args.viddecOutArgs.bytesConsumed = 0;  
    out_args.viddecOutArgs.outBufsInUseFlag = 0;
 
    return 0;

err:
    fprintf(stderr, "ERROR: An error has occured during ce_open() method\n");
    return -1;
}



static int mpeg4_decode(AVPacket *p)
{
    struct frame *f;
    uint8_t *buf;
    int bufsize;
    int err;
    int i;

   /* Ugly hack to solve the phys->virt translation in VIDDEC2_process function */
   if (firstTime) {
	firstTime = 0;
	for (i=0; i<num_frames; i++) {
		printf("CE_MPEG4: Registering buffer virt_dec=%lu virt=%p, phys=%p WITH SIZE=%u\n", (UInt32) frames[i].virt[0], frames[i].virt[0], (uint8_t *)frames[i].phys[0], ce_frame_size);
		Memory_registerContigBuf((UInt32) frames[i].virt[0], ce_frame_size, (UInt32)frames[i].phys[0]);
	}	
	Memory_registerContigBuf((UInt32) input_buf, INPUT_BUFFER_SIZE, Memory_getBufferPhysicalAddress(input_buf, INPUT_BUFFER_SIZE, NULL));
   }

    buf     = p->data;
    bufsize = ALIGN(p->size, 128); // According to the Mpeg4 Decoder User Guide

    /* Copy the compressed buffer (buf) to the allocated CMEM input buffer (input_buf) */
    memcpy(input_buf, buf, p->size);

    f = ofbp_get_frame();

    in_args.viddecInArgs.inputID  = (XDAS_Int32)f;
    in_args.viddecInArgs.numBytes = p->size;

    inbufs->descs[0].buf = (int8_t *)input_buf;
    inbufs->descs[0].bufSize = (XDAS_Int32) bufsize;

    pOutputBuf[0] = (uint8_t*)f->virt[0];
    outBufSize[0] = ce_frame_size;

    outbufs->bufs     = (XDAS_Int8 **)pOutputBuf;
    outbufs->bufSizes = (XDAS_Int32 *)outBufSize;
  
    err = VIDDEC2_process(dec, (XDM1_BufDesc *)inbufs, (XDM_BufDesc *)outbufs, (IVIDDEC2_InArgs *)&in_args.viddecInArgs, (IVIDDEC2_OutArgs *)&out_args);
    if (err) {
        fprintf(stderr, "VIDDEC2_process() error %d\n", err);
        //return -1;
    }
    
    /* Post the frame (Call the display manager that will enqueue and display the frame */
    for (i = 0; out_args.viddecOutArgs.freeBufID[i]; i++) {	
        f->x = out_args.viddecOutArgs.displayBufs->frameWidth;
        f->y = out_args.viddecOutArgs.displayBufs->frameHeight;
	/* Since only a single plane is used, the following can be used for the frame pitch */	
	f->linesize[1] = out_args.viddecOutArgs.displayBufs->framePitch;
        ofbp_post_frame((struct frame *)out_args.viddecOutArgs.freeBufID[i]);
    }

    /* Dereference the frame and eventually release it */
    for (i = 0; out_args.viddecOutArgs.freeBufID[i]; i++) {
        ofbp_put_frame((struct frame *)out_args.viddecOutArgs.freeBufID[i]);
    }

    return 0;
}


static void mpeg4_close(void)
{
    if (dec)        VIDDEC2_delete(dec);             dec    = NULL;
    if (inbufs)     free(inbufs);                inbufs     = NULL;
    if (outbufs)    free(outbufs);               outbufs    = NULL;
}

const struct codec mpeg4_codec = {
    .name   = "mpeg4dec",
    .flags  = OFBP_PHYS_MEM,
    .open   = mpeg4_open,
    .decode = mpeg4_decode,
    .close  = mpeg4_close,
};

