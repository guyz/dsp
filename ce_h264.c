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

#include "h264vdec.h"
#include "h264vdec_ti.h"

#include <libavcodec/avcodec.h>
#include "frame.h"
#include "codec.h"
#include "util.h"


#define INPUT_BUFFER_SIZE      1048576

AVCodecContext           *avc;
AVBitStreamFilterContext *bsf;

extern Engine_Handle    ce;
VIDDEC2_Handle          dec;
String 			decoderName_h264 = "h264dec";

IH264VDEC_Params          params;
IH264VDEC_DynamicParams   dynamicParams;
IH264VDEC_Status         status;
IH264VDEC_InArgs         in_args;
IH264VDEC_OutArgs        out_args;

XDAS_UInt8*  pOutputBuf [XDM_MAX_IO_BUFFERS];
XDAS_Int32   outBufSize [XDM_MAX_IO_BUFFERS];
XDAS_Int32   inBufSizes[5];
XDM1_BufDesc          *inbufs;
XDM_BufDesc           *outbufs;

extern uint8_t  *input_buf;
extern unsigned ce_frame_size;
extern unsigned num_frames;
extern struct frame *frames;
int fTime = 1;

static int h264_open(const char *name, AVCodecContext *cc,
                    struct frame_format *ff)
{
    XDAS_Int32 err;

    if (cc->codec_id != CODEC_ID_H264) {
        fprintf(stderr, "ERROR: unsupported CE codec %d\n", cc->codec_id);
        return -1;
    }


    if (cc->extradata && cc->extradata_size > 0 && cc->extradata[0] == 1) {
        bsf = av_bitstream_filter_init("h264_mp4toannexb");
        if (!bsf)
            return -1;
        avc = cc;
    }

    ff->width  = ALIGN(cc->width, 32);
    ff->height = ALIGN(cc->height, 16);

    ff->disp_x = 0;
    ff->disp_y = 0;
    ff->disp_w = cc->width;
    ff->disp_h = cc->height;
    ff->pixfmt = PIX_FMT_YUYV422;

    /* Base class sizes */
    params.viddecParams.size                  = sizeof(IH264VDEC_Params);
    status.viddecStatus.size   		      = sizeof(IH264VDEC_Status);
    dynamicParams.viddecDynamicParams.size    = sizeof(IH264VDEC_DynamicParams);
    in_args.viddecInArgs.size                 = sizeof(IVIDDEC2_InArgs);
    out_args.viddecOutArgs.size               = sizeof(IVIDDEC2_OutArgs);
	
    /* Init video decoder parameters */
    params.viddecParams.maxFrameRate    = 30;
    params.viddecParams.maxBitRate      = 10000000;
    params.viddecParams.dataEndianness  = XDM_BYTE;	
    params.viddecParams.maxHeight = 480;
    params.viddecParams.maxWidth = 864;
    params.viddecParams.forceChromaFormat = 4;

    IH264VDEC_Params *extRegParams = (IH264VDEC_Params *)&params.viddecParams;

    extRegParams->inputStreamFormat = 0; // 0 - byte, 1 - NAL
    extRegParams->maxDisplayDelay = 0;

    /* allocate and initialize video decoder on the engine */
    dec = VIDDEC2_create(ce, decoderName_h264, (IVIDDEC2_Params *)&params.viddecParams);
    if (dec == NULL) {
        fprintf(stderr, "ERROR: can't open codec %s\n", decoderName_h264);
        goto err;
    }  

    /* Set the dynamic parameters */
    dynamicParams.viddecDynamicParams.decodeHeader  = XDM_DECODE_AU; //XDM_DECODE_AU; //XDM_DECODE_AU; 
    dynamicParams.viddecDynamicParams.frameSkipMode = IVIDEO_NO_SKIP;
    dynamicParams.viddecDynamicParams.frameOrder = IVIDDEC2_DISPLAY_ORDER;
    dynamicParams.viddecDynamicParams.newFrameFlag = XDAS_FALSE;
    dynamicParams.viddecDynamicParams.mbDataFlag = XDAS_FALSE;


    IH264VDEC_DynamicParams *extParams = (IH264VDEC_DynamicParams *)&dynamicParams.viddecDynamicParams;
    extParams->mbErrorBufFlag  = FALSE;
    extParams->Sei_Vui_parse_flag = FALSE; 
    extParams->numNALunits = 0;

    err = VIDDEC2_control(dec, XDM_RESET, (IVIDDEC2_DynamicParams *)&dynamicParams, (IVIDDEC2_Status *)&status.viddecStatus);
    if (err) {
        fprintf(stderr, "VIDDEC2_control(XDM_RESET) failed %d\n", err);
        goto err;
    }

    err = VIDDEC2_control(dec, XDM_SETPARAMS, (IVIDDEC2_DynamicParams *)&dynamicParams, (IVIDDEC2_Status *)&status.viddecStatus);
    if (err) {
        fprintf(stderr, "VIDDEC2_control(XDM_SETPARAMS) failed %d\n", err);
        goto err;
    }

    /* Init buffers and context information */
    err = VIDDEC2_control(dec, XDM_GETBUFINFO, (IVIDDEC2_DynamicParams *)&dynamicParams, (IVIDDEC2_Status *)&status.viddecStatus);
    if (err) {
        fprintf(stderr, "VIDDEC2_control(XDM_GETBUFINFO) failed %d\n", err);
        goto err;
    }    

    inbufs = malloc(sizeof(*inbufs));
    outbufs = malloc(sizeof(*outbufs));

    inbufs->numBufs = status.viddecStatus.bufInfo.minNumInBufs;
    inbufs->descs[0].bufSize = status.viddecStatus.bufInfo.minInBufSize[0];

    outbufs->numBufs = 1;

    in_args.viddecInArgs.numBytes = 0;  
    out_args.viddecOutArgs.bytesConsumed = 0;  
    out_args.viddecOutArgs.outBufsInUseFlag = 0;
 
    return 0;

err:
    fprintf(stderr, "ERROR: An error has occured during ce_open() method\n");
    return -1;
}



static int h264_decode(AVPacket *p)
{
    struct frame *f;
    uint8_t *buf;
    int bufsize;
    int err;
    int i;

   /* Ugly hack to solve the phys->virt translation in VIDDEC2_process function */
   if (fTime) {
	fTime = 0;
	for (i=0; i<num_frames; i++) {
		printf("Registering buffer virt_dec=%lu virt=%p, phys=%p\n", (UInt32) frames[i].virt[0], frames[i].virt[0], (uint8_t *)frames[i].phys[0]);
		Memory_registerContigBuf((UInt32) frames[i].virt[0], ce_frame_size, (UInt32)frames[i].phys[0]);
	}	
   }

    if (bsf) {
        if (av_bitstream_filter_filter(bsf, avc, NULL, &buf, &bufsize,
                                       p->data, p->size, 0) < 0) {
            fprintf(stderr, "CE: bsf error\n");
            return -1;
        }
    } else {
        buf     = p->data;
        bufsize = p->size;
    }

    memcpy(input_buf, buf, bufsize);
    
    if (bsf)
        av_free(buf);

    f = ofbp_get_frame();

    in_args.viddecInArgs.inputID  = (XDAS_Int32)f;
    in_args.viddecInArgs.numBytes = bufsize;

    inbufs->descs[0].buf = (int8_t *)input_buf;
    inbufs->descs[0].bufSize = (XDAS_Int32) bufsize;

    pOutputBuf[0] = (uint8_t*)f->virt[0];
    outBufSize[0] = ce_frame_size;

    outbufs->bufs     = (XDAS_Int8 **)pOutputBuf;
    outbufs->bufSizes = (XDAS_Int32 *)outBufSize;
  
    err = VIDDEC2_process(dec, (XDM1_BufDesc *)inbufs, (XDM_BufDesc *)outbufs, (IVIDDEC2_InArgs *)&in_args.viddecInArgs, (IVIDDEC2_OutArgs *)&out_args);
    if (err) {
        fprintf(stderr, "VIDDEC2_process() error %d\n", err);
      //  return -1; //TODO: see why some videos generate errors. They are still displayed correctly. 
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


static void h264_close(void)
{
    if (dec)        VIDDEC2_delete(dec);             dec    = NULL;
    if (inbufs)     free(inbufs);                inbufs     = NULL;
    if (outbufs)    free(outbufs);               outbufs    = NULL;
    if (bsf)        av_bitstream_filter_close(bsf);  bsf    = NULL;
}

const struct codec h264_codec = {
    .name   = "h264dec",
    .flags  = OFBP_PHYS_MEM,
    .open   = h264_open,
    .decode = h264_decode,
    .close  = h264_close,
};

