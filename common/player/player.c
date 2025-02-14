// Copyright 2025 Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "player.c"

#include "player.h"
#include "common.h"
#include <byteswap.h>
#include <rk_debug.h>
#include <rk_mpi_adec.h>
#include <rk_mpi_ao.h>
#include <rk_mpi_cal.h>
#include <rk_mpi_mb.h>
#include <rk_mpi_mmz.h>
#include <rk_mpi_rgn.h>
#include <rk_mpi_sys.h>
#include <rk_mpi_tde.h>
#include <rk_mpi_vdec.h>
#include <rk_mpi_vo.h>
#include <rk_mpi_vpss.h>
#include <rkdemuxer.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mp3dec.h>

#define SLOW_MOTION_RATE 4

#define VDEC_CHN_ID 0
#define ADEC_CHN_ID 0
#define AO_CHN_ID 0
#define AO_DEV_ID 0

#define JPEG_VDEC_CHN_ID 1
#define JPEG_VPSS_GRP_ID 2
#define JPEG_VPSS_CHN_ID 0

typedef enum {
	UNINITIALIZED,
	PAUSE,
	RUNNING,
} PLAYER_STATUS;

typedef struct _rkMpiVDECCtx {
	int s32ChnFd;
	bool bEnableMbPool;
	MB_POOL s32Pool;
	uint32_t u32Width;
	uint32_t u32Height;
} VDEC_CTX_S;

typedef struct _rkPlayerCtx {
	char input_file_path[128];
	VDEC_CTX_S vdec_ctx;
	DemuxerParam demuxer_param;
	bool enable_audio;
	bool enable_video;
	void *demuxer_handle;
	rk_player_event_cb event_cb;
	pthread_mutex_t mutex; // protect rkdemuxer
	pthread_t video_player_thread_id;
	pthread_t audio_player_thread_id;
	PLAYER_STATUS video_status;
	PLAYER_STATUS audio_status;
	int64_t video_current_pts;
	int64_t audio_current_pts;
	int64_t video_total_duraion;
	int64_t audio_total_duraion;
} PLAYER_CTX_S;

#define MP3MAXFRAMESIZE 4608

typedef struct _MP3DecInfo {
	/* pointers to platform-specific data structures */
	void *FrameHeaderPS;
	void *SideInfoPS;
	void *ScaleFactorInfoPS;
	void *HuffmanInfoPS;
	void *DequantInfoPS;
	void *IMDCTInfoPS;
	void *SubbandInfoPS;

	/* buffer which must be large enough to hold largest possible main_data section */
	unsigned char mainBuf[MAINBUF_SIZE];

	/* special info for "free" bitrate files */
	int freeBitrateFlag;
	int freeBitrateSlots;

	/* user-accessible info */
	int bitrate;
	int nChans;
	int samprate;
	int nGrans;     /* granules per frame */
	int nGranSamps; /* samples per granule */
	int nSlots;
	int layer;
	int vbr;         /* is vbr flag */
	uint32_t fSize;  /* file length, no tags */
	uint32_t fCount; /* frame count */
	char TOC[100];   /* TOC */

	MPEGVersion version;

	int mainDataBegin;
	int mainDataBytes;

	int part23Length[MAX_NGRAN][MAX_NCHAN];
} MP3DecInfo;

typedef struct {
	HMP3Decoder pMp3Dec;
	MP3FrameInfo frameInfo;
	RK_U8 decMp3buf[MP3MAXFRAMESIZE];
	short int decPCMbuf[MP3MAXFRAMESIZE];
	RK_U64 audioPts;
} RK_ADEC_MP3_CTX_S;

static int extCodecHandle = -1;
static unsigned mp3DecInitCnt = 0;

static int mp3_decoder_open(void *pDecoderAttr, void **ppDecoder) {
	if (pDecoderAttr == NULL) {
		LOG_ERROR("pDecoderAttr is NULL");
		return RK_FAILURE;
	}

	RK_ADEC_MP3_CTX_S *ctx = (RK_ADEC_MP3_CTX_S *)malloc(sizeof(RK_ADEC_MP3_CTX_S));
	if (!ctx) {
		LOG_ERROR("malloc adec mp3 ctx failed");
		return RK_FAILURE;
	}

	memset(ctx, 0, sizeof(RK_ADEC_MP3_CTX_S));
	ctx->pMp3Dec = MP3InitDecoder();
	if (!ctx->pMp3Dec) {
		LOG_ERROR("malloc adec pMp3Dec failed");
		free(ctx);
		ctx = NULL;
		return RK_FAILURE;
	}

	memset(ctx->decMp3buf, 0, sizeof(ctx->decMp3buf));
	memset(ctx->decPCMbuf, 0, sizeof(ctx->decPCMbuf));

	*ppDecoder = (void *)ctx;
	return RK_SUCCESS;
}

static int mp3_decoder_close(void *pDecoder) {
	RK_ADEC_MP3_CTX_S *ctx = (RK_ADEC_MP3_CTX_S *)pDecoder;
	if (ctx == NULL)
		return RK_FAILURE;

	if (ctx->pMp3Dec) {
		MP3FreeDecoder(ctx->pMp3Dec);
		ctx->pMp3Dec = NULL;
	}

	if (ctx)
		free(ctx);

	return RK_SUCCESS;
}

static int mp3_decoder_decode(void *pDecoder, void *pDecParam) {
	RK_S32 ret = 0;
	RK_ADEC_MP3_CTX_S *ctx = (RK_ADEC_MP3_CTX_S *)pDecoder;
	RK_S32 skip = 0;

	if (ctx == NULL || ctx->pMp3Dec == NULL || pDecParam == NULL)
		return ADEC_DECODER_ERROR;

	AUDIO_ADENC_PARAM_S *pParam = (AUDIO_ADENC_PARAM_S *)pDecParam;
	RK_BOOL eos = RK_FALSE;
	RK_U8 *pInput = NULL;
	RK_S32 inLength = pParam->u32InLen;
	RK_S32 copySize = 0;
	RK_U64 calcPts = 0;

	if ((pParam->pu8InBuf == NULL) || (inLength == 0))
		eos = RK_TRUE;

	copySize = (inLength <= MP3MAXFRAMESIZE) ? inLength : MP3MAXFRAMESIZE;
	inLength -= copySize;
	memcpy(ctx->decMp3buf, pParam->pu8InBuf, copySize);
	pInput = (RK_U8 *)ctx->decMp3buf;
	if (copySize && (skip = MP3FindSyncWord(pInput, copySize)) < 0) {
		LOG_ERROR("mp3 decode don't find sync word\n");
		pParam->u32InLen = 0;
		return ADEC_DECODER_ERROR;
	}

	copySize -= skip;
	pInput += skip;

	ret = MP3Decode(ctx->pMp3Dec, &pInput, &copySize, ctx->decPCMbuf, 0);
	pParam->u32InLen = inLength + copySize;

	if (ret) {
		if (eos)
			return ADEC_DECODER_EOS;

		pParam->u32InLen = inLength;
		if (ret == ERR_MP3_INDATA_UNDERFLOW) {
			LOG_WARN("mp3 decode input data underflow");
			return ADEC_DECODER_ERROR;
		}

		if (ret == -2) {
			LOG_WARN("mp3 encoded data does not start from the first frame\n");
		} else {
			LOG_ERROR("mp3 decode error, ret = %d", ret);
			return ADEC_DECODER_ERROR;
		}
	}

	MP3GetLastFrameInfo(ctx->pMp3Dec, &ctx->frameInfo);

	MP3DecInfo *mp3DecInfo = (MP3DecInfo *)ctx->pMp3Dec;
	pParam->u32OutLen = mp3DecInfo->nGrans * mp3DecInfo->nGranSamps * ctx->frameInfo.nChans * 2;
	memcpy(pParam->pu8OutBuf, (RK_U8 *)ctx->decPCMbuf, pParam->u32OutLen);

	calcPts = (((RK_U64)pParam->u32OutLen / 2) * 1000000) / ctx->frameInfo.samprate;
	if (pParam->u64InTimeStamp)
		pParam->u64OutTimeStamp = pParam->u64InTimeStamp;
	else if (ctx->audioPts)
		pParam->u64OutTimeStamp = ctx->audioPts + calcPts;

	if (pParam->u64OutTimeStamp)
		ctx->audioPts = pParam->u64OutTimeStamp;
	else {
		ctx->audioPts += calcPts;
	}

	return ADEC_DECODER_OK;
}

static int mp3_decoder_get_frame_info(void *pDecoder, void *pInfo) {
	ADEC_FRAME_INFO_S stFrameInfo;
	RK_ADEC_MP3_CTX_S *ctx = (RK_ADEC_MP3_CTX_S *)pDecoder;

	if (ctx == NULL || pInfo == NULL)
		return RK_FAILURE;

	MP3GetLastFrameInfo(ctx->pMp3Dec, &ctx->frameInfo);

	if (!ctx->frameInfo.bitsPerSample) {
		LOG_ERROR("mp3 decode get info failed");
		return RK_FAILURE;
	}

	memset(&stFrameInfo, 0, sizeof(ADEC_FRAME_INFO_S));
	stFrameInfo.u32Channels = ctx->frameInfo.nChans;
	stFrameInfo.u32SampleRate = ctx->frameInfo.samprate;
	stFrameInfo.enBitWidth = AUDIO_BIT_WIDTH_16;
	memcpy(pInfo, &stFrameInfo, sizeof(ADEC_FRAME_INFO_S));
	return RK_SUCCESS;
}

static int register_adec_mp3() {
	if (!mp3DecInitCnt) {
		ADEC_DECODER_S adecCtx;
		memset(&adecCtx, 0, sizeof(ADEC_DECODER_S));

		extCodecHandle = -1;
		adecCtx.enType = RK_AUDIO_ID_MP3;
		snprintf((RK_CHAR *)(adecCtx.aszName), sizeof(adecCtx.aszName), "rkaudio1");
		adecCtx.pfnOpenDecoder = mp3_decoder_open;
		adecCtx.pfnDecodeFrm = mp3_decoder_decode;
		adecCtx.pfnGetFrmInfo = mp3_decoder_get_frame_info;
		adecCtx.pfnCloseDecoder = mp3_decoder_close;
		adecCtx.pfnResetDecoder = RK_NULL;

		LOG_DEBUG("register ext audio decoder\n");
		RK_S32 ret = RK_MPI_ADEC_RegisterDecoder(&extCodecHandle, &adecCtx);
		if (ret != RK_SUCCESS) {
			LOG_ERROR("adec register decoder fail, ret = 0x%x", ret);
			return RK_FAILURE;
		}
	}

	mp3DecInitCnt++;
	return RK_SUCCESS;
}

static int unregister_adec_mp3() {
	if (extCodecHandle == -1)
		return RK_SUCCESS;

	if (0 == mp3DecInitCnt) {
		return 0;
	} else if (1 == mp3DecInitCnt) {
		LOG_DEBUG("unregister ext audio decoder\n");
		int ret = RK_MPI_ADEC_UnRegisterDecoder(extCodecHandle);
		if (ret != RK_SUCCESS) {
			LOG_ERROR("adec unregister decoder fail, ret = 0x%x", ret);
			return RK_FAILURE;
		}

		extCodecHandle = -1;
	}

	mp3DecInitCnt--;
	return RK_SUCCESS;
}

static int create_vo(void) {
	int ret;
	VO_PUB_ATTR_S VoPubAttr;
	VO_VIDEO_LAYER_ATTR_S stLayerAttr;
	VO_CSC_S VideoCSC;
	VO_CHN_ATTR_S VoChnAttr;
	uint32_t u32DispBufLen;
	uint32_t display_width = rk_param_get_int("display:width", 240);
	uint32_t display_height = rk_param_get_int("display:height", 320);
	uint32_t vo_dev = rk_param_get_int("display:dev_id", 1);
	uint32_t vo_layer = rk_param_get_int("display:layer_id", 5);
	uint32_t vo_chn = rk_param_get_int("display:play_chn_id", 1);
	uint32_t fps = rk_param_get_int("display:fps", 30);
	const char *intf_type = rk_param_get_string("display:intf_type", "LCD");

#ifndef DRAW_UI_BY_VO
	memset(&VoPubAttr, 0, sizeof(VO_PUB_ATTR_S));
	memset(&stLayerAttr, 0, sizeof(VO_VIDEO_LAYER_ATTR_S));
	memset(&VideoCSC, 0, sizeof(VO_CSC_S));
	memset(&VoChnAttr, 0, sizeof(VoChnAttr));
	if (!strcmp(intf_type, "LCD"))
		VoPubAttr.enIntfType = VO_INTF_LCD;
	else if (!strcmp(intf_type, "MIPI"))
		VoPubAttr.enIntfType = VO_INTF_MIPI;
	else if (!strcmp(intf_type, "DP"))
		VoPubAttr.enIntfType = VO_INTF_DP;
	else {
		LOG_ERROR("bad intf_type!\n");
		return -1;
	}
	VoPubAttr.enIntfSync = VO_OUTPUT_DEFAULT;

	ret = RK_MPI_VO_SetPubAttr(vo_dev, &VoPubAttr);
	if (ret)
		LOG_ERROR("RK_MPI_VO_SetPubAttr failed %#X\n", ret);
	ret = RK_MPI_VO_Enable(vo_dev);
	if (ret)
		LOG_ERROR("RK_MPI_VO_Enable failed %#X\n", ret);
	RK_MPI_VO_GetLayerDispBufLen(vo_layer, &u32DispBufLen);
	LOG_INFO("Get vo_layer %d disp buf len is %d.\n", vo_layer, u32DispBufLen);
	u32DispBufLen = 3;
	ret = RK_MPI_VO_SetLayerDispBufLen(vo_layer, u32DispBufLen);
	LOG_INFO("Agin Get vo_layer %d disp buf len is %d.\n", vo_layer, u32DispBufLen);

	ret = RK_MPI_VO_GetPubAttr(vo_dev, &VoPubAttr);
	if ((VoPubAttr.stSyncInfo.u16Hact == 0) || (VoPubAttr.stSyncInfo.u16Vact == 0)) {
		VoPubAttr.stSyncInfo.u16Hact = display_width;
		VoPubAttr.stSyncInfo.u16Vact = display_height;
	}

	stLayerAttr.stDispRect.s32X = 0;
	stLayerAttr.stDispRect.s32Y = 0;
	stLayerAttr.stDispRect.u32Width = display_width;
	stLayerAttr.stDispRect.u32Height = display_height;
	stLayerAttr.stImageSize.u32Width = display_width;
	stLayerAttr.stImageSize.u32Height = display_height;
	LOG_INFO("stLayerAttr W=%d, H=%d\n", stLayerAttr.stDispRect.u32Width,
	         stLayerAttr.stDispRect.u32Height);

	stLayerAttr.u32DispFrmRt = fps;
	stLayerAttr.enPixFormat = RK_FMT_YUV420SP;
	VideoCSC.enCscMatrix = VO_CSC_MATRIX_IDENTITY;
	VideoCSC.u32Contrast = 50;
	VideoCSC.u32Hue = 50;
	VideoCSC.u32Luma = 50;
	VideoCSC.u32Satuature = 50;

	/*bind layer0 to device hd0*/
	ret = RK_MPI_VO_BindLayer(vo_layer, vo_dev, VO_LAYER_MODE_GRAPHIC);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("failed to bind layer\n");
		return ret;
	}
	ret = RK_MPI_VO_SetLayerAttr(vo_layer, &stLayerAttr);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("failed to set layer attr\n");
		return ret;
	}
	ret = RK_MPI_VO_SetLayerSpliceMode(vo_layer, VO_SPLICE_MODE_GPU);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("failed to set layer splice mode\n");
		return ret;
	}
	ret = RK_MPI_VO_EnableLayer(vo_layer);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("failed to enable layer\n");
		return ret;
	}
	ret = RK_MPI_VO_SetLayerCSC(vo_layer, &VideoCSC);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("failed to set layer csc\n");
		return ret;
	}
#endif
	VoChnAttr.bDeflicker = RK_FALSE;
	VoChnAttr.u32Priority = rk_param_get_int("display:play_chn_priority", 2);
	VoChnAttr.stRect.s32X = 0;
	VoChnAttr.stRect.s32Y = 0;
	VoChnAttr.stRect.u32Width = display_width;
	VoChnAttr.stRect.u32Height = display_height;
	VoChnAttr.enRotation = rk_param_get_int("display:rotation", 1);
	ret = RK_MPI_VO_SetChnAttr(vo_layer, vo_chn, &VoChnAttr);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("set %d layer %d ch vo attr failed!\n", vo_layer, vo_chn);
		return ret;
	}
	ret = RK_MPI_VO_EnableChn(vo_layer, vo_chn);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("create %d layer %d ch vo failed!\n", vo_layer, vo_chn);
		return ret;
	}
	LOG_INFO("create vo success\n");

	return ret;
}

static void destroy_vo(void) {
	int ret = 0;
	uint32_t vo_dev = rk_param_get_int("display:dev_id", 1);
	uint32_t vo_layer = rk_param_get_int("display:layer_id", 5);
	uint32_t vo_chn = rk_param_get_int("display:play_chn_id", 1);
	ret = RK_MPI_VO_DisableChn(vo_layer, vo_chn);
	if (ret) {
		LOG_ERROR("RK_MPI_VO_DisableChn failed, ret is %#x\n", ret);
		return;
	}
#ifndef DRAW_UI_BY_VO
	ret = RK_MPI_VO_DisableLayer(vo_layer);
	if (ret) {
		LOG_ERROR("RK_MPI_VO_DisableLayer failed, ret is %#x\n", ret);
		return;
	}
	ret = RK_MPI_VO_Disable(vo_dev);
	if (ret) {
		LOG_ERROR("RK_MPI_VO_Disable failed, ret is %#x\n", ret);
		return;
	}
	ret = RK_MPI_VO_UnBindLayer(vo_layer, vo_dev);
	if (ret) {
		LOG_ERROR("RK_MPI_VO_UnBindLayer failed, ret is %#x\n", ret);
		return;
	}
#endif
}

static int create_vdec(VDEC_CTX_S *ctx) {
	int ret = RK_SUCCESS;
	VDEC_CHN_ATTR_S stAttr;
	VDEC_CHN_PARAM_S stVdecParam;
	MB_POOL_CONFIG_S stMbPoolCfg;
	VDEC_PIC_BUF_ATTR_S stVdecPicBufAttr;
	MB_PIC_CAL_S stMbPicCalret;
	VDEC_MOD_PARAM_S stModParam;
	const char *tmp_output_data_type = rk_param_get_string("video.0:output_data_type", NULL);

	memset(&stAttr, 0, sizeof(VDEC_CHN_ATTR_S));
	memset(&stVdecParam, 0, sizeof(VDEC_CHN_PARAM_S));
	memset(&stModParam, 0, sizeof(VDEC_MOD_PARAM_S));

	if (ctx->bEnableMbPool) {
		stModParam.enVdecMBSource = MB_SOURCE_USER;
		ret = RK_MPI_VDEC_SetModParam(&stModParam);
		if (ret != RK_SUCCESS) {
			LOG_ERROR("vdec %d SetModParam failed! errcode %x\n", VDEC_CHN_ID, ret);
			return ret;
		}
	}
	stAttr.enMode = VIDEO_MODE_STREAM;
	if (tmp_output_data_type == NULL) {
		LOG_ERROR("bad video file format!\n");
		return -1;
	} else if (!strcmp(tmp_output_data_type, "H.264")) {
		stAttr.enType = RK_VIDEO_ID_AVC;
	} else if (!strcmp(tmp_output_data_type, "H.265")) {
		stAttr.enType = RK_VIDEO_ID_HEVC;
	} else {
		LOG_ERROR("unknown video file input!\n");
		return -1;
	}
	stAttr.u32PicWidth = ctx->u32Width;
	stAttr.u32PicHeight = ctx->u32Height;
	stAttr.u32FrameBufCnt = 8;
	stAttr.u32StreamBufCnt = rk_param_get_int("video.0:buf_cnt", 11);

	ret = RK_MPI_VDEC_CreateChn(VDEC_CHN_ID, &stAttr);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("create %d vdec failed! ", VDEC_CHN_ID);
		return ret;
	}

	stVdecParam.stVdecPictureParam.enPixelFormat = RK_FMT_YUV420SP;
	stVdecParam.stVdecVideoParam.enCompressMode = COMPRESS_MODE_NONE;

	ret = RK_MPI_VDEC_SetChnParam(VDEC_CHN_ID, &stVdecParam);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("set chn %d param failed %x! ", VDEC_CHN_ID, ret);
		return ret;
	}

	ctx->s32ChnFd = RK_MPI_VDEC_GetFd(VDEC_CHN_ID);
	if (ctx->s32ChnFd <= 0) {
		LOG_ERROR("get fd chn %d failed %d", VDEC_CHN_ID, ctx->s32ChnFd);
		return ret;
	}

	if (ctx->bEnableMbPool) {
		memset(&stMbPoolCfg, 0, sizeof(MB_POOL_CONFIG_S));

		stVdecPicBufAttr.enCodecType = stAttr.enType;
		stVdecPicBufAttr.stPicBufAttr.u32Width = ctx->u32Width;
		stVdecPicBufAttr.stPicBufAttr.u32Height = ctx->u32Height;
		stVdecPicBufAttr.stPicBufAttr.enPixelFormat = RK_FMT_YUV420SP;
		stVdecPicBufAttr.stPicBufAttr.enCompMode = COMPRESS_MODE_NONE;
		ret = RK_MPI_CAL_VDEC_GetPicBufferSize(&stVdecPicBufAttr, &stMbPicCalret);
		if (ret != RK_SUCCESS) {
			LOG_ERROR("get picture buffer size failed. err 0x%x", ret);
			return ret;
		}

		stMbPoolCfg.u64MBSize = stMbPicCalret.u32MBSize;
		stMbPoolCfg.u32MBCnt = 10;
		stMbPoolCfg.enRemapMode = MB_REMAP_MODE_CACHED;
		stMbPoolCfg.bPreAlloc = RK_TRUE;
		ctx->s32Pool = RK_MPI_MB_CreatePool(&stMbPoolCfg);
		if (ctx->s32Pool == MB_INVALID_POOLID) {
			LOG_ERROR("create pool failed!");
			return ret;
		}
		ret = RK_MPI_VDEC_AttachMbPool(VDEC_CHN_ID, ctx->s32Pool);
		if (ret != RK_SUCCESS) {
			LOG_ERROR("attatc vdec mb pool %d failed! ", VDEC_CHN_ID);
			return ret;
		}
	}

	ret = RK_MPI_VDEC_StartRecvStream(VDEC_CHN_ID);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("start recv chn %d failed %x! ", VDEC_CHN_ID, ret);
		return ret;
	}
	LOG_INFO("create vdec success\n");
	return ret;
}

static void destroy_vdec(VDEC_CTX_S *ctx) {
	RK_MPI_VDEC_StopRecvStream(VDEC_CHN_ID);

	if (ctx->s32ChnFd > 0) {
		RK_MPI_VDEC_CloseFd(VDEC_CHN_ID);
	}

	if (ctx->bEnableMbPool) {
		RK_MPI_VDEC_DetachMbPool(VDEC_CHN_ID);
		RK_MPI_VDEC_DestroyChn(VDEC_CHN_ID);
		RK_MPI_MB_DestroyPool(ctx->s32Pool);
	} else {
		RK_MPI_VDEC_DestroyChn(VDEC_CHN_ID);
	}
	return;
}

static int create_ao(void) {
	int ret = 0;
	int sample_rate = rk_param_get_int("audio.0:sample_rate", 16000);
	int channels = rk_param_get_int("audio.0:channels", 2);
	const char *card_name = rk_param_get_string("audio.0:card_name", "hw:0,0");
	const char *format = rk_param_get_string("audio.0:format", NULL);
	AUDIO_DEV aoDevId = 0;
	AO_CHN aoChn = 0;
	AIO_ATTR_S aoAttr;
	AO_CHN_PARAM_S pstParams;

	memset(&pstParams, 0, sizeof(AO_CHN_PARAM_S));
	memset(&aoAttr, 0, sizeof(AIO_ATTR_S));
	sprintf((char *)aoAttr.u8CardName, "%s", card_name);

	aoAttr.soundCard.channels = channels;
	aoAttr.soundCard.sampleRate = sample_rate;
	aoAttr.soundCard.bitWidth = AUDIO_BIT_WIDTH_16;

	if (!strcmp(format, "S16")) {
		aoAttr.enBitwidth = AUDIO_BIT_WIDTH_16;
	} else if (!strcmp(format, "U8")) {
		aoAttr.enBitwidth = AUDIO_BIT_WIDTH_8;
	} else {
		LOG_ERROR("not support %s\n", format);
		return -1;
	}
	aoAttr.enSamplerate = (AUDIO_SAMPLE_RATE_E)sample_rate;

	if (channels == 1)
		aoAttr.enSoundmode = AUDIO_SOUND_MODE_MONO;
	else if (channels == 2)
		aoAttr.enSoundmode = AUDIO_SOUND_MODE_STEREO;
	else {
		LOG_ERROR("unsupport = %d", channels);
		return RK_FAILURE;
	}

	aoAttr.u32PtNumPerFrm = rk_param_get_int("audio.0:frame_size", 1024);
	aoAttr.u32FrmNum = 4;
	aoAttr.u32EXFlag = 0;
	aoAttr.u32ChnCnt = 2;

	RK_MPI_AO_SetPubAttr(aoDevId, &aoAttr);
	ret = RK_MPI_AO_Enable(aoDevId);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("RK_MPI_AO_Enable failed %#X\n", ret);
		return RK_FAILURE;
	}
	/*==============================================================================*/
	pstParams.enLoopbackMode = AUDIO_LOOPBACK_NONE;
	ret = RK_MPI_AO_SetChnParams(aoDevId, aoChn, &pstParams);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("ao set channel params, aoChn = %d", aoChn);
		return RK_FAILURE;
	}

	if (channels == 1)
		RK_MPI_AO_SetTrackMode(aoDevId, AUDIO_TRACK_OUT_STEREO);
	else
		RK_MPI_AO_SetTrackMode(aoDevId, AUDIO_TRACK_NORMAL);
	/*==============================================================================*/
	ret = RK_MPI_AO_EnableChn(aoDevId, aoChn);
	if (ret != 0) {
		LOG_ERROR("ao enable channel fail, aoChn = %d, reason = %x", aoChn, ret);
		return RK_FAILURE;
	}
	/*==============================================================================*/
	// set sample rate of input data
	ret = RK_MPI_AO_EnableReSmp(aoDevId, aoChn, (AUDIO_SAMPLE_RATE_E)sample_rate);
	if (ret != 0) {
		LOG_ERROR("ao enable channel fail, reason = %x, aoChn = %d", ret, aoChn);
		return RK_FAILURE;
	}

	ret = RK_MPI_AO_SetVolume(aoDevId, rk_param_get_int("audio.0:volume", 50));
	if (ret != 0) {
		LOG_ERROR("RK_MPI_AO_SetVolume failed %#X\n", ret);
		return RK_FAILURE;
	}
	LOG_INFO("create ao success\n");

	return RK_SUCCESS;
}

static int destroy_ao(void) {
	RK_MPI_AO_DisableReSmp(0, 0);
	RK_MPI_AO_DisableChn(0, 0);
	RK_MPI_AO_Disable(0);
}

static int create_adec(void) {
	int ret = 0;
	int sample_rate = rk_param_get_int("audio.0:sample_rate", 16000);
	int channels = rk_param_get_int("audio.0:channels", 2);
	const char *format = rk_param_get_string("audio.0:format", NULL);
	const char *encode_type = rk_param_get_string("audio.0:encode_type", NULL);
	ADEC_CHN_ATTR_S pstChnAttr;
	memset(&pstChnAttr, 0, sizeof(ADEC_CHN_ATTR_S));

	ret = register_adec_mp3();
	if (ret != 0) {
		LOG_ERROR("register_adec_mp3 failed %#X\n", ret);
		return -1;
	}
	if (!strcmp(encode_type, "MP2")) {
		pstChnAttr.enType = RK_AUDIO_ID_MP2;
		pstChnAttr.stCodecAttr.enType = RK_AUDIO_ID_MP2;
	} else if (!strcmp(encode_type, "G711A")) {
		pstChnAttr.enType = RK_AUDIO_ID_PCM_ALAW;
		pstChnAttr.stCodecAttr.enType = RK_AUDIO_ID_PCM_ALAW;
	} else if (!strcmp(encode_type, "MP3")) {
		pstChnAttr.enType = RK_AUDIO_ID_MP3;
		pstChnAttr.stCodecAttr.enType = RK_AUDIO_ID_MP3;
	} else {
		LOG_ERROR("not support %s\n", encode_type);
		return -1;
	}
	pstChnAttr.stCodecAttr.u32Channels = channels; // default 1
	pstChnAttr.stCodecAttr.u32SampleRate = sample_rate;

	pstChnAttr.u32BufCount = 64;
	pstChnAttr.u32BufSize = 50 * 1024;
	ret = RK_MPI_ADEC_CreateChn(ADEC_CHN_ID, &pstChnAttr);
	if (ret) {
		LOG_ERROR("create adec chn %d err:0x%x\n", ADEC_CHN_ID, ret);
		ret = RK_FAILURE;
	}
	LOG_INFO("create adec success\n");
	return ret;
}

static int destroy_adec(void) {
	RK_MPI_ADEC_DestroyChn(ADEC_CHN_ID);
	unregister_adec_mp3();
	return 0;
}

static int bind_vdec_vo(void) {
	int ret = 0;
	MPP_CHN_S stSrcChn, stDestChn;
	uint32_t vo_layer = rk_param_get_int("display:layer_id", 5);
	uint32_t vo_chn = rk_param_get_int("display:play_chn_id", 1);
	stSrcChn.enModId = RK_ID_VDEC;
	stSrcChn.s32DevId = 0;
	stSrcChn.s32ChnId = VDEC_CHN_ID;

	stDestChn.enModId = RK_ID_VO;
	stDestChn.s32DevId = vo_layer;
	stDestChn.s32ChnId = vo_chn;

	ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("vdec bind vo fail:%x", ret);
		return -1;
	}
	return 0;
}

static int unbind_vdec_vo(void) {
	int ret = 0;
	MPP_CHN_S stSrcChn, stDestChn;
	uint32_t vo_layer = rk_param_get_int("display:layer_id", 5);
	uint32_t vo_chn = rk_param_get_int("display:play_chn_id", 1);
	stSrcChn.enModId = RK_ID_VDEC;
	stSrcChn.s32DevId = 0;
	stSrcChn.s32ChnId = VDEC_CHN_ID;

	stDestChn.enModId = RK_ID_VO;
	stDestChn.s32DevId = vo_layer;
	stDestChn.s32ChnId = vo_chn;

	ret = RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("vdec unbind vo fail:%x", ret);
		return -1;
	}
	return 0;
}

static int bind_adec_ao(void) {
	int ret = 0;
	MPP_CHN_S stSrcChn, stDestChn;
	stSrcChn.enModId = RK_ID_ADEC;
	stSrcChn.s32DevId = 0;
	stSrcChn.s32ChnId = 0;

	stDestChn.enModId = RK_ID_AO;
	stDestChn.s32DevId = 0;
	stDestChn.s32ChnId = 0;

	ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("adec bind ao fail:%x\n", ret);
		return -1;
	}
	return 0;
}

static int unbind_adec_ao(void) {
	int ret = 0;
	MPP_CHN_S stSrcChn, stDestChn;
	stSrcChn.enModId = RK_ID_ADEC;
	stSrcChn.s32DevId = 0;
	stSrcChn.s32ChnId = 0;

	stDestChn.enModId = RK_ID_AO;
	stDestChn.s32DevId = 0;
	stDestChn.s32ChnId = 0;

	ret = RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("adec unbind ao fail:%x\n", ret);
		return -1;
	}
	return 0;
}

static int get_jpeg_resolution(uint8_t *buf, uint32_t buf_size, uint32_t *width, uint32_t *height) {
	uint32_t cur = 0;
	if (buf[0] != 0xFF || buf[1] != 0xD8) {
		LOG_ERROR("Invalid jpeg data\n");
		return -1;
	}
	cur += 2;
	while (cur + 4 + 4 < buf_size) {
		if (buf[cur] != 0xFF) {
			LOG_ERROR("Bad Jpg file, 0xFF expected at offset 0x%x\n", cur);
			break;
		}

		if (buf[cur + 1] == 0xC0 || buf[cur + 1] == 0xC2) {
			cur += 5;
			*height = bswap_16(*(uint16_t *)(buf + cur));
			cur += 2;
			*width = bswap_16(*(uint16_t *)(buf + cur));
			LOG_INFO("w * h[%d, %d]\n", *width, *height);
			return 0;
		}
		cur += 2;
		cur += bswap_16(*(uint16_t *)(buf + cur));
	}
	LOG_ERROR("Can't get resolution\n");
	return -1;
}

static bool validate_format(uint32_t format) {
	if (format == RK_FMT_BGRA8888 || format == RK_FMT_RGBA8888 || format == RK_FMT_RGB565 ||
	    format == RK_FMT_YUV420SP)
		return true;
	return false;
};

static uint32_t cal_size_by_format(uint32_t width, uint32_t height, uint32_t format) {
	uint32_t size = 0;
	switch (format) {
	case RK_FMT_YUV420SP:
		size = width * height * 3 / 2;
		break;
	case RK_FMT_RGB565:
		size = width * height * 2;
		break;
	case RK_FMT_RGBA8888:
	case RK_FMT_BGRA8888:
		size = width * height * 4;
		break;
	default:
		break;
	}
	return size;
}

static int create_jpeg_vdec(const RK_PHOTO_CONFIG *config, RK_PHOTO_DATA *data) {
	int ret = RK_SUCCESS;
	VDEC_CHN_ATTR_S stAttr;
	VDEC_CHN_PARAM_S stVdecParam;
	MB_POOL_CONFIG_S stMbPoolCfg;
	VDEC_PIC_BUF_ATTR_S stVdecPicBufAttr;
	MB_PIC_CAL_S stMbPicCalret;
	VDEC_MOD_PARAM_S stModParam;

	memset(&stAttr, 0, sizeof(VDEC_CHN_ATTR_S));
	memset(&stVdecParam, 0, sizeof(VDEC_CHN_PARAM_S));
	memset(&stModParam, 0, sizeof(VDEC_MOD_PARAM_S));

	stAttr.enMode = VIDEO_MODE_FRAME;
	stAttr.enType = RK_VIDEO_ID_JPEG;
	stAttr.u32PicWidth = config->input_width;   // width of jpeg
	stAttr.u32PicHeight = config->input_height; // height of jpeg
	stAttr.u32FrameBufCnt = 8;
	stAttr.u32StreamBufCnt = 3;

	ret = RK_MPI_VDEC_CreateChn(JPEG_VDEC_CHN_ID, &stAttr);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("create %d vdec failed! ", JPEG_VDEC_CHN_ID);
		return ret;
	}

	stVdecParam.stVdecPictureParam.enPixelFormat = RK_FMT_YUV420SP;

	ret = RK_MPI_VDEC_SetChnParam(JPEG_VDEC_CHN_ID, &stVdecParam);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("set chn %d param failed %x! ", JPEG_VDEC_CHN_ID, ret);
		return ret;
	}

	ret = RK_MPI_VDEC_StartRecvStream(JPEG_VDEC_CHN_ID);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("start recv chn %d failed %x! ", JPEG_VDEC_CHN_ID, ret);
		return ret;
	}
	LOG_INFO("create vdec success\n");
	return ret;
}

static void destroy_jpeg_vdec(void) {
	RK_MPI_VDEC_StopRecvStream(JPEG_VDEC_CHN_ID);
	RK_MPI_VDEC_DestroyChn(JPEG_VDEC_CHN_ID);
	return;
}

// Use vpss to resize and change image format.
static int create_jpeg_vpss(const RK_PHOTO_CONFIG *config, RK_PHOTO_DATA *data) {
	int ret = 0;
	VPSS_GRP_ATTR_S stVpssGrpAttr;
	VPSS_CHN_ATTR_S stVpssChnAttr;
	memset(&stVpssGrpAttr, 0, sizeof(stVpssGrpAttr));
	memset(&stVpssChnAttr, 0, sizeof(stVpssChnAttr));
	stVpssGrpAttr.u32MaxW = 4096;
	stVpssGrpAttr.u32MaxH = 4096;
	stVpssGrpAttr.enPixelFormat = RK_FMT_YUV420SP;
	stVpssGrpAttr.stFrameRate.s32SrcFrameRate = -1;
	stVpssGrpAttr.stFrameRate.s32DstFrameRate = -1;
	stVpssGrpAttr.enCompressMode = COMPRESS_MODE_NONE;
	ret = RK_MPI_VPSS_CreateGrp(JPEG_VPSS_GRP_ID, &stVpssGrpAttr);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("RK_MPI_VPSS_CreateGrp error! ret is %#x\n", ret);
		return ret;
	}

	stVpssChnAttr.enChnMode = VPSS_CHN_MODE_AUTO;
	stVpssChnAttr.enDynamicRange = DYNAMIC_RANGE_SDR8;
	stVpssChnAttr.enPixelFormat = config->output_format;
	stVpssChnAttr.stFrameRate.s32SrcFrameRate = -1;
	stVpssChnAttr.stFrameRate.s32DstFrameRate = -1;
	stVpssChnAttr.u32Width = config->output_width;
	stVpssChnAttr.u32Height = config->output_height;
	stVpssChnAttr.enCompressMode = COMPRESS_MODE_NONE;
	stVpssChnAttr.u32Depth = 1;
	ret = RK_MPI_VPSS_SetChnAttr(JPEG_VPSS_GRP_ID, JPEG_VPSS_CHN_ID, &stVpssChnAttr);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("0: RK_MPI_VPSS_SetChnAttr error! ret is %#x\n", ret);
		return ret;
	}
	ret = RK_MPI_VPSS_EnableChn(JPEG_VPSS_GRP_ID, JPEG_VPSS_CHN_ID);
	if (ret != RK_SUCCESS)
		LOG_ERROR("0: RK_MPI_VPSS_EnableChn error! ret is %#x\n", ret);
	RK_MPI_VPSS_SetVProcDev(JPEG_VPSS_GRP_ID, VIDEO_PROC_DEV_GPU);
	ret = RK_MPI_VPSS_StartGrp(JPEG_VPSS_GRP_ID);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("RK_MPI_VPSS_StartGrp error! ret is %#x\n", ret);
		return ret;
	}
	LOG_INFO("create vpss success\n");
	return ret;
}

static int destroy_jpeg_vpss(void) {
	RK_MPI_VPSS_StopGrp(JPEG_VPSS_GRP_ID);
	RK_MPI_VPSS_DisableChn(JPEG_VPSS_GRP_ID, JPEG_VPSS_CHN_ID);
	RK_MPI_VPSS_DestroyGrp(JPEG_VPSS_GRP_ID);
	return 0;
}

static int send_jpeg_to_vdec(RK_PHOTO_DATA *data, MB_BLK input_mb, RK_U32 input_size) {
	int ret = 0;
	MPP_CHN_S stSrcChn, stDestChn;
	VDEC_STREAM_S input_frame;
	VIDEO_FRAME_INFO_S output_frame;
	stSrcChn.enModId = RK_ID_VDEC;
	stSrcChn.s32DevId = 0;
	stSrcChn.s32ChnId = JPEG_VDEC_CHN_ID;

	stDestChn.enModId = RK_ID_VPSS;
	stDestChn.s32DevId = JPEG_VPSS_GRP_ID;
	stDestChn.s32ChnId = JPEG_VPSS_CHN_ID;

	ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("vdec bind vpss error %#X\n", ret);
		return ret;
	}

	memset(&input_frame, 0, sizeof(input_frame));
	input_frame.u64PTS = 0;
	input_frame.pMbBlk = input_mb;
	input_frame.u32Len = input_size;
	input_frame.bEndOfStream = RK_TRUE;
	input_frame.bEndOfFrame = RK_TRUE;
	input_frame.bBypassMbBlk = RK_FALSE;
	RK_MPI_SYS_MmzFlushCache(input_mb, false);

	ret = RK_MPI_VDEC_SendStream(JPEG_VDEC_CHN_ID, &input_frame, 2000);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("RK_MPI_VDEC_SendStream failed %#X\n", ret);
		RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
		return ret;
	}
	ret = RK_MPI_VPSS_GetChnFrame(JPEG_VPSS_GRP_ID, JPEG_VPSS_CHN_ID, &output_frame, 2000);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("RK_MPI_VPSS_GetChnFrame failed %#X\n", ret);
		RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
		return ret;
	}
	LOG_INFO("get frame width %d, height %d, format %X, size %d\n", output_frame.stVFrame.u32Width,
	         output_frame.stVFrame.u32Height, output_frame.stVFrame.enPixelFormat,
	         cal_size_by_format(output_frame.stVFrame.u32Width, output_frame.stVFrame.u32Height,
	                            output_frame.stVFrame.enPixelFormat));
	data->width = output_frame.stVFrame.u32Width;
	data->height = output_frame.stVFrame.u32Height;
	data->format = output_frame.stVFrame.enPixelFormat;
	data->size = cal_size_by_format(output_frame.stVFrame.u32Width, output_frame.stVFrame.u32Height,
	                                output_frame.stVFrame.enPixelFormat);
	data->data = malloc(data->size);
	if (!data) {
		LOG_ERROR("malloc failed\n");
		ret = -1;
	} else {
		RK_MPI_SYS_MmzFlushCache(output_frame.stVFrame.pMbBlk, false);
		memcpy(data->data, RK_MPI_MB_Handle2VirAddr(output_frame.stVFrame.pMbBlk), data->size);
	}
	RK_MPI_VPSS_ReleaseChnFrame(JPEG_VPSS_GRP_ID, JPEG_VPSS_CHN_ID, &output_frame);
	RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
	LOG_INFO("get jpeg success\n");
	return 0;
}

static int decode_one_jpeg(RK_PHOTO_CONFIG *config, RK_PHOTO_DATA *data, MB_BLK input_mb,
                           RK_U32 input_size) {
	int ret = 0;
	ret = create_jpeg_vdec(config, data);
	if (ret != 0)
		return ret;
	ret = create_jpeg_vpss(config, data);
	if (ret != 0) {
		destroy_jpeg_vdec();
		return ret;
	}
	ret = send_jpeg_to_vdec(data, input_mb, input_size);
	destroy_jpeg_vpss();
	destroy_jpeg_vdec();
	return ret;
}

static int buffer_free_cb(void *opaque) {
	if (opaque) {
		free(opaque);
		opaque = NULL;
	}
	return 0;
}

static void read_video_packet_cb(void *handle) {
	DemuxerPacket *video_packet = (DemuxerPacket *)handle;
	PLAYER_CTX_S *ctx = (PLAYER_CTX_S *)video_packet->ptr;
	int ret = 0;
	VDEC_STREAM_S video_stream;
	MB_BLK buffer = NULL;
	MB_EXT_CONFIG_S mb_ext_config;

	RKIPC_CHECK_POINTER(video_packet, );
	RKIPC_CHECK_POINTER(handle, );
	LOG_DEBUG("packet size %d, data %p, eof %d, pts %lld\n", video_packet->s32PacketSize,
	          video_packet->s8PacketData, video_packet->s8EofFlag, video_packet->s64Pts);
	if (!video_packet->s8EofFlag) {
		memset(&mb_ext_config, 0, sizeof(MB_EXT_CONFIG_S));
		mb_ext_config.pFreeCB = buffer_free_cb;
		mb_ext_config.pOpaque = (void *)video_packet->s8PacketData;
		mb_ext_config.pu8VirAddr = (RK_U8 *)video_packet->s8PacketData;
		mb_ext_config.u64Size = video_packet->s32PacketSize;

		RK_MPI_SYS_CreateMB(&buffer, &mb_ext_config);

		video_stream.u64PTS = video_packet->s64Pts;
		video_stream.pMbBlk = buffer;
		video_stream.u32Len = video_packet->s32PacketSize;
		video_stream.bEndOfStream = video_packet->s8EofFlag ? RK_TRUE : RK_FALSE;
		video_stream.bEndOfFrame = video_packet->s8EofFlag ? RK_TRUE : RK_FALSE;
		video_stream.bBypassMbBlk = RK_TRUE;

	__RETRY:
		ret = RK_MPI_VDEC_SendStream(VDEC_CHN_ID, &video_stream, 1000);
		if (ret) {
			LOG_ERROR("RK_MPI_VDEC_SendStream failed[%x]", ret);
			usleep(1000llu);
			goto __RETRY;
		}
		RK_MPI_MB_ReleaseMB(video_stream.pMbBlk);
		ctx->video_current_pts = video_packet->s64Pts;
	} else {
		if (video_packet->s8PacketData) {
			free(video_packet->s8PacketData);
			video_packet->s8PacketData = NULL;
		}
	}
	return;
}

static void read_audio_packet_cb(void *handle) {
	int ret = 0;
	MB_EXT_CONFIG_S mb_ext_config;
	DemuxerPacket *audio_packet = (DemuxerPacket *)handle;
	AUDIO_STREAM_S audio_stream;
	PLAYER_CTX_S *ctx = (PLAYER_CTX_S *)audio_packet->ptr;

	RKIPC_CHECK_POINTER(audio_packet, );
	RKIPC_CHECK_POINTER(handle, );
	LOG_DEBUG("packet size %d, data %p, eof %d, pts %lld\n", audio_packet->s32PacketSize,
	          audio_packet->s8PacketData, audio_packet->s8EofFlag, audio_packet->s64Pts);
	if (audio_packet->s8EofFlag) {
		RK_MPI_ADEC_SendEndOfStream(ADEC_CHN_ID, RK_FALSE);
	} else {
		audio_stream.u32Len = audio_packet->s32PacketSize;
		audio_stream.u64TimeStamp = audio_packet->s64Pts;
		audio_stream.u32Seq = audio_packet->s32Series;
		audio_stream.bBypassMbBlk = RK_TRUE;

		memset(&mb_ext_config, 0, sizeof(MB_EXT_CONFIG_S));
		mb_ext_config.pFreeCB = buffer_free_cb;
		mb_ext_config.pOpaque = (void *)audio_packet->s8PacketData;
		mb_ext_config.pu8VirAddr = (RK_U8 *)audio_packet->s8PacketData;
		mb_ext_config.u64Size = audio_packet->s32PacketSize;
		RK_MPI_SYS_CreateMB(&(audio_stream.pMbBlk), &mb_ext_config);

	__RETRY:
		ret = RK_MPI_ADEC_SendStream(ADEC_CHN_ID, &audio_stream, RK_TRUE);
		if (ret != RK_SUCCESS) {
			LOG_ERROR("RK_MPI_ADEC_SendStream failed[%x]", ret);
			goto __RETRY;
		}
		RK_MPI_MB_ReleaseMB(audio_stream.pMbBlk);
		ctx->audio_current_pts = audio_packet->s64Pts;
	}

	return;
}

static void *video_player_loop(void *p) {
	PLAYER_CTX_S *ctx = (PLAYER_CTX_S *)p;
	int ret = 0;
	bool quit = false;
	uint32_t fps = ctx->demuxer_param.s32VideoAvgFrameRate;
	uint64_t frame_interval_time = 0, cost_time = 0, delay_time = 0;
	DemuxerPacket packet;
	struct timespec start_time, end_time;

	prctl(PR_SET_NAME, "video_player_loop", 0, 0, 0);

	LOG_INFO("enter thread\n");
	if (fps == 0) {
		LOG_ERROR("bad parameter from video file!\n");
		return NULL;
	}
	frame_interval_time = (1000.0 / fps) * 1000;

	while (!quit) {
		switch (ctx->video_status) {
		case UNINITIALIZED:
			LOG_INFO("exit thread\n");
			quit = true;
			break;
		case PAUSE:
			usleep(300 * 1000);
			break;
		case RUNNING:
			clock_gettime(CLOCK_MONOTONIC, &start_time);
			pthread_mutex_lock(&ctx->mutex);
			// Packet malloc by rkdemuxer, free by user. So this critial zone is safe.
			ret = rkdemuxer_read_one_video_packet(ctx->demuxer_handle, &packet);
			if (ret) {
				LOG_ERROR("rkdemuxer_read_one_video_packet failed\n");
				ctx->video_status = PAUSE;
				if (ctx->event_cb)
					ctx->event_cb(RK_PLAYER_ERROR);
			} else {
				if (packet.s8EofFlag) {
					LOG_INFO("reach EOS, stop play and reset file position\n");
					rkdemuxer_seek_video_pts(ctx->demuxer_handle, 0);
					ctx->video_current_pts = 0;
					ctx->video_status = PAUSE;
					if (ctx->event_cb) {
						ctx->event_cb(RK_PLAYER_EOF);
					}
				}
			}
			pthread_mutex_unlock(&ctx->mutex);
			if (!ret)
				read_video_packet_cb(&packet);
			clock_gettime(CLOCK_MONOTONIC, &end_time);
			cost_time = end_time.tv_sec * 1000000LL + end_time.tv_nsec / 1000LL
				- (start_time.tv_sec * 1000000LL + start_time.tv_nsec / 1000LL);
			if (cost_time < frame_interval_time) {
				delay_time = frame_interval_time - cost_time;
				usleep(delay_time);
				LOG_DEBUG("frame interval %llu ms, decode cost %llu ms, delay %llu ms\n"
						, frame_interval_time / 1000
						, cost_time / 1000
						, delay_time / 1000
						);
			} else {
				LOG_ERROR("decode one frame cost too much time %u ms\n", cost_time / 1000);
			}
		}
	}
	return NULL;
}

static void *audio_player_loop(void *p) {
	PLAYER_CTX_S *ctx = (PLAYER_CTX_S *)p;
	int ret = 0;
	int sample_rate = rk_param_get_int("audio.0:sample_rate", 16000);
	int channels = rk_param_get_int("audio.0:channels", 2);
	int frame_size = rk_param_get_int("audio.0:frame_size", 1024);
	const char *format = rk_param_get_string("audio.0:format", NULL);
	bool quit = false;
	uint64_t frame_interval_time = 0, cost_time = 0, delay_time = 0;
	DemuxerPacket packet;
	struct timespec start_time, end_time;

	prctl(PR_SET_NAME, "audio_player_loop", 0, 0, 0);
	LOG_INFO("enter thread\n");

	if (!strcmp(format, "S16")) {
		frame_interval_time = ((uint64_t)frame_size * 1000 * 1000) / sample_rate / channels / 2;
	} else if (!strcmp(format, "U8")) {
		frame_interval_time = ((uint64_t)frame_size * 1000 * 1000) / sample_rate / channels;
	} else {
		LOG_ERROR("not support %s\n", format);
		return NULL;
	}
	LOG_INFO("sample_rate %d, channels %d, frame_size %d, frame interval %llu\n"
			, sample_rate, channels, frame_size, frame_interval_time);
	while (!quit) {
		switch (ctx->audio_status) {
		case UNINITIALIZED:
			LOG_INFO("exit thread\n");
			quit = true;
			return NULL;
		case PAUSE:
			usleep(300 * 1000);
			break;
		case RUNNING:
			clock_gettime(CLOCK_MONOTONIC, &start_time);
			pthread_mutex_lock(&ctx->mutex);
			// Packet malloc by rkdemuxer, free by user. So this critial zone is safe.
			ret = rkdemuxer_read_one_audio_packet(ctx->demuxer_handle, &packet);
			if (ret) {
				LOG_ERROR("rkdemuxer_read_one_audio_packet failed\n");
				ctx->audio_status = PAUSE;
			} else {
				if (packet.s8EofFlag) {
					LOG_INFO("reach EOS, stop play and reset file position\n");
					rkdemuxer_seek_audio_pts(ctx->demuxer_handle, 0);
					ctx->audio_status = PAUSE;
					ctx->audio_current_pts = 0;
				}
			}

			pthread_mutex_unlock(&ctx->mutex);
			if (!ret)
				read_audio_packet_cb(&packet);
			clock_gettime(CLOCK_MONOTONIC, &end_time);
			cost_time = end_time.tv_sec * 1000000LL + end_time.tv_nsec / 1000LL
				- (start_time.tv_sec * 1000000LL + start_time.tv_nsec / 1000LL);
			if (cost_time < frame_interval_time) {
				delay_time = frame_interval_time - cost_time;
				usleep(delay_time);
				LOG_DEBUG("frame interval %llu ms, decode cost %llu ms, delay %llu ms\n"
						, frame_interval_time / 1000
						, cost_time / 1000
						, delay_time / 1000
						);
			} else {
				LOG_ERROR("decode one frame cost too much time %u ms\n", cost_time / 1000);
			}
		}
	}
	return NULL;
}

static int create_media_pipe(PLAYER_CTX_S *ctx) {
	int ret = 0;

	pthread_mutex_init(&ctx->mutex, NULL);
	if (ctx->enable_video) {
		ctx->vdec_ctx.bEnableMbPool = true;
		ret = create_vo();
		if (ret)
			return ret;
		ret = create_vdec(&ctx->vdec_ctx);
		if (ret)
			return ret;
		ret = bind_vdec_vo();
		if (ret)
			return ret;
	}

	if (ctx->enable_audio) {
		ret = create_ao();
		if (ret)
			return ret;
		ret = create_adec();
		if (ret)
			return ret;
		ret = bind_adec_ao();
		if (ret)
			return ret;
		pthread_create(&ctx->audio_player_thread_id, NULL, audio_player_loop, (void *)ctx);
	}
	LOG_INFO("create player success!\n");
	return 0;
}

static int destroy_media_pipe(PLAYER_CTX_S *ctx) {
	int ret = 0;

	rkdemuxer_deinit(&ctx->demuxer_handle);
	if (ctx->enable_video) {
		unbind_vdec_vo();
		destroy_vdec(&ctx->vdec_ctx);
		destroy_vo();
	}
	if (ctx->enable_audio) {
		unbind_adec_ao();
		destroy_adec();
		destroy_ao();
	}
	pthread_mutex_destroy(&ctx->mutex);
	LOG_INFO("destroy player success!\n");
	return 0;
}

// INFO: rkdemuxer_read_xxxx_duration() would travese the record file, so reinit the demuxer after
// read duration is a robust choice.
static int read_duration_unlocked(PLAYER_CTX_S *ctx, const char *input_file_path) {
	int64_t duration;
	int ret = 0;
	DemuxerInput stDemuxerInput;
	DemuxerParam tmp_param;
	void *tmp_handle = NULL;
	if (ctx->audio_status != UNINITIALIZED && ctx->enable_audio) {
		LOG_ERROR("need remove file before set new file\n");
		return -1;
	}
	if (ctx->video_status != UNINITIALIZED && ctx->enable_video) {
		LOG_ERROR("need remove file before set new file\n");
		return -1;
	}
	memset(ctx->input_file_path, 0, sizeof(ctx->input_file_path));
	memcpy(ctx->input_file_path, input_file_path, sizeof(ctx->input_file_path));
	memset(&stDemuxerInput, 0, sizeof(DemuxerInput));
	stDemuxerInput.ptr = (void *)ctx;
	stDemuxerInput.s8ReadModeFlag = true;
	stDemuxerInput.s8VideoEnableFlag = ctx->enable_video;
	stDemuxerInput.s8AudioEnableFlag = ctx->enable_audio;
	ret = rkdemuxer_init(&tmp_handle, &stDemuxerInput);
	if (ret) {
		LOG_ERROR("rkdemuxer init failed\n");
		return -1;
	}

	memset(&tmp_param, 0, sizeof(tmp_param));
	ret = rkdemuxer_get_param(tmp_handle, ctx->input_file_path, &tmp_param);
	if (ret) {
		LOG_ERROR("rkdemuxer get param failed\n");
		rkdemuxer_deinit(&tmp_handle);
		return -1;
	}

	usleep(100 * 1000);
	if (tmp_param.pVideoCodec) {
		rkdemuxer_read_video_duration(tmp_handle, &duration);
		ctx->video_total_duraion = duration;
		LOG_INFO("get video duration %lld\n", duration);
	}
	if (tmp_param.pAudioCodec) {
		rkdemuxer_read_audio_duration(tmp_handle, &duration);
		ctx->audio_total_duraion = duration;
		LOG_INFO("get audio duration %lld\n", duration);
	}
	rkdemuxer_deinit(&tmp_handle);
	return 0;
}

static int set_record_file_unlocked(PLAYER_CTX_S *ctx, const char *input_file_path) {
	int ret = 0;
	DemuxerInput stDemuxerInput;
	if (ctx->audio_status != UNINITIALIZED && ctx->enable_audio) {
		LOG_ERROR("need remove file before set new file\n");
		return -1;
	}
	if (ctx->video_status != UNINITIALIZED && ctx->enable_video) {
		LOG_ERROR("need remove file before set new file\n");
		return -1;
	}
	memset(ctx->input_file_path, 0, sizeof(ctx->input_file_path));
	memcpy(ctx->input_file_path, input_file_path, sizeof(ctx->input_file_path));
	memset(&stDemuxerInput, 0, sizeof(DemuxerInput));
	stDemuxerInput.ptr = (void *)ctx;
	stDemuxerInput.s8ReadModeFlag = true;
	stDemuxerInput.s8VideoEnableFlag = ctx->enable_video;
	stDemuxerInput.s8AudioEnableFlag = ctx->enable_audio;
	ret = rkdemuxer_init(&ctx->demuxer_handle, &stDemuxerInput);
	if (ret) {
		LOG_ERROR("rkdemuxer init failed\n");
		return -1;
	}

	memset(&ctx->demuxer_param, 0, sizeof(ctx->demuxer_param));
	ret = rkdemuxer_get_param(ctx->demuxer_handle, ctx->input_file_path, &ctx->demuxer_param);
	if (ret) {
		LOG_ERROR("rkdemuxer get param failed\n");
		rkdemuxer_deinit(&ctx->demuxer_handle);
		return -1;
	}

	LOG_INFO("demuxer file %s\n", ctx->input_file_path);
	LOG_INFO("total time %d ms, width %d, height %d, format %d\n", ctx->video_total_duraion / 1000,
	         ctx->demuxer_param.s32VideoWidth, ctx->demuxer_param.s32VideoHeigh,
	         ctx->demuxer_param.s8VideoFormat);
	LOG_INFO("video codec %s, avg fps %d, first pts %lld, timebase %d/%d\n",
	         ctx->demuxer_param.pVideoCodec, ctx->demuxer_param.s32VideoAvgFrameRate,
	         ctx->demuxer_param.s64VideoFirstPTS, ctx->demuxer_param.s32VideoTimeBaseDen,
	         ctx->demuxer_param.s32VideoTimeBaseNum);
	LOG_INFO("audio codec %s, channel %d, sample rate %d, first pts %lld, timebase %d/%d\n",
	         ctx->demuxer_param.pAudioCodec, ctx->demuxer_param.s32AudioChannels,
	         ctx->demuxer_param.s32AudioSampleRate, ctx->demuxer_param.s64AudioFirstPTS,
	         ctx->demuxer_param.s32AudioTimeBaseDen, ctx->demuxer_param.s32AudioTimeBaseNum);

	if (ctx->enable_video && ctx->demuxer_param.pVideoCodec) {
		ctx->video_status = PAUSE;
		pthread_create(&ctx->video_player_thread_id, NULL, video_player_loop, (void *)ctx);
	}
	if (ctx->enable_audio && ctx->demuxer_param.pAudioCodec) {
		ctx->audio_status = PAUSE;
		pthread_create(&ctx->audio_player_thread_id, NULL, audio_player_loop, (void *)ctx);
	}
	return 0;
}

static int remove_record_file_unlocked(PLAYER_CTX_S *ctx) {
	if (ctx->video_status == UNINITIALIZED && ctx->audio_status == UNINITIALIZED)
		return 0;
	ctx->video_status = UNINITIALIZED;
	ctx->audio_status = UNINITIALIZED;
	ctx->video_total_duraion = 0;
	ctx->audio_total_duraion = 0;
	if (ctx->enable_video)
		pthread_join(ctx->video_player_thread_id, NULL);
	if (ctx->enable_audio)
		pthread_join(ctx->audio_player_thread_id, NULL);
	rkdemuxer_deinit(&ctx->demuxer_handle);
	return 0;
}

int rk_player_create(RK_PLAYER_CONFIG_S *config) {
	LOG_INFO("enter\n");
	RKIPC_CHECK_POINTER(config, -1);
	if (!config->enable_audio && !config->enable_video) {
		LOG_ERROR("bad config parameter!\n");
		return -1;
	}
	if (!config->input_width || !config->input_height) {
		LOG_ERROR("bad input resolution %dx%d!\n", config->input_width, config->input_height);
		return -1;
	}

	PLAYER_CTX_S *ctx = malloc(sizeof(PLAYER_CTX_S));
	if (!ctx) {
		LOG_ERROR("malloc failed\n");
		return -1;
	}
	memset(ctx, 0, sizeof(PLAYER_CTX_S));
	ctx->enable_video = config->enable_video;
	ctx->enable_audio = config->enable_audio;
	ctx->event_cb = config->event_cb;
	ctx->vdec_ctx.u32Width = config->input_width;
	ctx->vdec_ctx.u32Height = config->input_height;
	if (create_media_pipe(ctx) != 0) {
		LOG_ERROR("initialize failed\n");
		free(ctx);
		return -1;
	}
	config->ctx = ctx;
	return 0;
}

int rk_player_set_file(RK_PLAYER_CONFIG_S *config, const char *input_file_path) {
	int ret = 0;
	LOG_INFO("enter\n");
	RKIPC_CHECK_POINTER(config, -1);
	RKIPC_CHECK_POINTER(input_file_path, -1);
	PLAYER_CTX_S *ctx = (PLAYER_CTX_S *)config->ctx;
	RKIPC_CHECK_POINTER(ctx, -1);
	if (access(input_file_path, F_OK) != 0) {
		LOG_ERROR("can't access file %s\n", input_file_path);
		return -1;
	}
	pthread_mutex_lock(&ctx->mutex);
	remove_record_file_unlocked(ctx);
	read_duration_unlocked(ctx, input_file_path);
	ret = set_record_file_unlocked(ctx, input_file_path);
	pthread_mutex_unlock(&ctx->mutex);
	return ret;
}

int rk_player_play(RK_PLAYER_CONFIG_S *config) {
	int ret = 0;
	LOG_INFO("enter\n");
	RKIPC_CHECK_POINTER(config, -1);
	PLAYER_CTX_S *ctx = (PLAYER_CTX_S *)config->ctx;
	RKIPC_CHECK_POINTER(ctx, -1);
	pthread_mutex_lock(&ctx->mutex);
	if (ctx->enable_video) {
		if (ctx->video_status == UNINITIALIZED) {
			ret = -1;
			LOG_ERROR("change video_status in a uninitialized player!\n");
		} else {
			ctx->video_status = RUNNING;
		}
	}
	if (ctx->enable_audio) {
		if (ctx->audio_status == UNINITIALIZED) {
			LOG_ERROR("change audio_status in a uninitialized player!\n");
		} else {
			ctx->audio_status = RUNNING;
		}
	}
	pthread_mutex_unlock(&ctx->mutex);
	if (!ret && ctx->event_cb)
		ctx->event_cb(RK_PLAYER_PLAY);
	return ret;
}

int rk_player_pause(RK_PLAYER_CONFIG_S *config) {
	int ret = 0;
	LOG_INFO("enter\n");
	RKIPC_CHECK_POINTER(config, -1);
	PLAYER_CTX_S *ctx = (PLAYER_CTX_S *)config->ctx;
	RKIPC_CHECK_POINTER(ctx, -1);
	pthread_mutex_lock(&ctx->mutex);
	if (ctx->enable_video) {
		if (ctx->video_status == UNINITIALIZED) {
			ret = -1;
			LOG_ERROR("change video_status in a uninitialized player!\n");
		} else if (ctx->video_status == RUNNING) {
			ctx->video_status = PAUSE;
		}
	}
	if (ctx->enable_audio) {
		if (ctx->audio_status == UNINITIALIZED) {
			LOG_ERROR("change audio_status in a uninitialized player!\n");
		} else if (ctx->audio_status == RUNNING) {
			ctx->audio_status = PAUSE;
		}
	}
	pthread_mutex_unlock(&ctx->mutex);
	if (!ret && ctx->event_cb)
		ctx->event_cb(RK_PLAYER_PAUSE);
	return ret;
}

int rk_player_stop(RK_PLAYER_CONFIG_S *config) {
	int ret = 0;
	LOG_INFO("enter\n");
	RKIPC_CHECK_POINTER(config, -1);
	PLAYER_CTX_S *ctx = (PLAYER_CTX_S *)config->ctx;
	RKIPC_CHECK_POINTER(ctx, -1);
	ret = rk_player_pause(config);
	ret |= rk_player_video_seek(config, 0);
	ret |= rk_player_audio_seek(config, 0);
	if (ctx->event_cb)
		ctx->event_cb(RK_PLAYER_STOP);

	return ret;
}

int rk_player_destroy(RK_PLAYER_CONFIG_S *config) {
	LOG_INFO("enter\n");
	RKIPC_CHECK_POINTER(config, -1);
	PLAYER_CTX_S *ctx = (PLAYER_CTX_S *)config->ctx;
	RKIPC_CHECK_POINTER(ctx, -1);
	pthread_mutex_lock(&ctx->mutex);
	remove_record_file_unlocked(ctx);
	pthread_mutex_unlock(&ctx->mutex);
	destroy_media_pipe(ctx);
	free(ctx);
	return 0;
}

int rk_player_video_seek(RK_PLAYER_CONFIG_S *config, uint64_t time_ms) {
	int ret = 0;
	LOG_INFO("enter\n");
	RKIPC_CHECK_POINTER(config, -1);
	PLAYER_CTX_S *ctx = (PLAYER_CTX_S *)config->ctx;
	RKIPC_CHECK_POINTER(ctx, -1);
	pthread_mutex_lock(&ctx->mutex);
	if (ctx->enable_video) {
		if (ctx->video_status == UNINITIALIZED) {
			ret = -1;
			LOG_ERROR("change video_status in a uninitialized player!\n");
		} else if (ctx->video_status == RUNNING) {
			ret = -1;
			LOG_ERROR("need to pause player before seek!\n");
		} else {
			ctx->video_status = PAUSE;
			ret = rkdemuxer_seek_video_pts(ctx->demuxer_handle, time_ms * 1000);
			if (ret) {
				LOG_ERROR("seek video pts 0 failed\n");
				ctx->video_status = PAUSE;
			} else {
				ctx->video_current_pts = time_ms * 1000;
			}
		}
	}
	pthread_mutex_unlock(&ctx->mutex);
	return ret;
}

int rk_player_audio_seek(RK_PLAYER_CONFIG_S *config, uint64_t time_ms) {
	int ret = 0;
	LOG_INFO("enter\n");
	RKIPC_CHECK_POINTER(config, -1);
	PLAYER_CTX_S *ctx = (PLAYER_CTX_S *)config->ctx;
	RKIPC_CHECK_POINTER(ctx, -1);
	pthread_mutex_lock(&ctx->mutex);
	if (ctx->enable_audio) {
		if (ctx->audio_status == UNINITIALIZED) {
			ret = -1;
			LOG_ERROR("change audio_status in a uninitialized player!\n");
		} else if (ctx->audio_status == RUNNING) {
			ret = -1;
			LOG_ERROR("need to pause player before seek!\n");
		} else {
			ctx->audio_status = PAUSE;
			ret = rkdemuxer_seek_audio_pts(ctx->demuxer_handle, time_ms * 1000);
			if (ret) {
				LOG_ERROR("seek audio pts 0 failed\n");
				ctx->audio_status = PAUSE;
			} else {
				ctx->audio_current_pts = time_ms * 1000;
			}
		}
	}
	pthread_mutex_unlock(&ctx->mutex);
	return ret;
}

int rk_player_get_video_duration(RK_PLAYER_CONFIG_S *config, uint32_t *duration) {
	int ret = 0;
	LOG_INFO("enter\n");
	RKIPC_CHECK_POINTER(config, -1);
	RKIPC_CHECK_POINTER(duration, -1);
	PLAYER_CTX_S *ctx = (PLAYER_CTX_S *)config->ctx;
	RKIPC_CHECK_POINTER(ctx, -1);
	*duration = (uint32_t)ctx->video_total_duraion / 1000;
	return 0;
}

int rk_player_get_audio_duration(RK_PLAYER_CONFIG_S *config, uint32_t *duration) {
	int ret = 0;
	LOG_INFO("enter\n");
	RKIPC_CHECK_POINTER(config, -1);
	RKIPC_CHECK_POINTER(duration, -1);
	PLAYER_CTX_S *ctx = (PLAYER_CTX_S *)config->ctx;
	RKIPC_CHECK_POINTER(ctx, -1);
	pthread_mutex_lock(&ctx->mutex);
	*duration = (uint32_t)ctx->audio_total_duraion / 1000;
	pthread_mutex_unlock(&ctx->mutex);
	return 0;
}

int rk_player_get_video_position(RK_PLAYER_CONFIG_S *config, uint32_t *position) {
	int ret = 0;
	LOG_INFO("enter\n");
	RKIPC_CHECK_POINTER(config, -1);
	PLAYER_CTX_S *ctx = (PLAYER_CTX_S *)config->ctx;
	RKIPC_CHECK_POINTER(ctx, -1);
	pthread_mutex_lock(&ctx->mutex);
	if (ctx->enable_video)
		*position = (ctx->video_current_pts / 1000);
	pthread_mutex_unlock(&ctx->mutex);
	return 0;
}

int rk_player_get_audio_position(RK_PLAYER_CONFIG_S *config, uint32_t *position) {
	int ret = 0;
	LOG_INFO("enter\n");
	RKIPC_CHECK_POINTER(config, -1);
	PLAYER_CTX_S *ctx = (PLAYER_CTX_S *)config->ctx;
	RKIPC_CHECK_POINTER(ctx, -1);
	pthread_mutex_lock(&ctx->mutex);
	if (ctx->enable_audio)
		*position = (ctx->audio_current_pts / 1000);
	pthread_mutex_unlock(&ctx->mutex);
	return 0;
}

int rk_player_get_photo(RK_PHOTO_CONFIG *config, RK_PHOTO_DATA *data) {
	RKIPC_CHECK_POINTER(config, -1);
	RKIPC_CHECK_POINTER(data, -1);
	RKIPC_CHECK_POINTER(config->file_path, -1);
	if (config->output_width == 0 || config->output_height == 0) {
		LOG_ERROR("invalid output size");
		return -1;
	}
	if (!validate_format(config->output_format)) {
		LOG_ERROR("invalid output format\n");
		return -1;
	}
	int fd = -1;
	int ret = 0;
	uint32_t width, height;
	uint32_t buf_size = 0;
	uint8_t *buf = NULL;
	MB_BLK mb_buffer = NULL;
	struct stat statbuf;
	MB_EXT_CONFIG_S mb_ext_config;

	// Parse jpeg file.
	fd = open(config->file_path, O_RDONLY);
	if (fd < 0) {
		LOG_ERROR("open file %s error: %s\n", config->file_path, strerror(errno));
		return -1;
	}
	ret = fstat(fd, &statbuf);
	if (ret != 0) {
		LOG_ERROR("get file %s stat error: %s\n", config->file_path, strerror(errno));
		close(fd);
		return -1;
	}
	buf_size = statbuf.st_size;
	buf = malloc(buf_size);
	if (!buf) {
		LOG_ERROR("malloc error\n");
		close(fd);
	}
	ret = read(fd, buf, buf_size);
	if (ret <= 0) {
		LOG_ERROR("read error %s\n", strerror(errno));
		close(fd);
		free(buf);
		return -1;
	}
	if (config->input_width == 0 || config->input_height == 0) {
		ret = get_jpeg_resolution(buf, buf_size, &config->input_width, &config->input_height);
		if (ret != 0) {
			LOG_ERROR("get jpeg resolution failed\n");
			close(fd);
			free(buf);
			return -1;
		}
	}
	LOG_INFO("read jpeg %s: width %u, height %u, jpeg size %u\n", config->file_path,
	         config->input_width, config->input_height, buf_size);

	// Prepare vdec input frame.
	memset(&mb_ext_config, 0, sizeof(MB_EXT_CONFIG_S));
	mb_ext_config.pFreeCB = buffer_free_cb;
	mb_ext_config.pOpaque = (void *)buf;
	mb_ext_config.pu8VirAddr = (RK_U8 *)buf;
	mb_ext_config.u64Size = buf_size;
	ret = RK_MPI_SYS_CreateMB(&mb_buffer, &mb_ext_config);
	if (ret != 0) {
		LOG_ERROR("RK_MPI_SYS_CreateMB failed %#X\n", ret);
		close(fd);
		free(buf);
		return -1;
	}
	// Decode jpeg.
	ret = decode_one_jpeg(config, data, mb_buffer, buf_size);
	if (ret != 0)
		LOG_ERROR("decode jpeg failed\n");
	RKIPC_CHECK_POINTER(data, -1);

	RK_MPI_MB_ReleaseMB(mb_buffer);
	close(fd);
	return ret;
}

int rk_player_release_photo(RK_PHOTO_DATA *data) {
	RKIPC_CHECK_POINTER(data, -1);
	RKIPC_CHECK_POINTER(data->data, -1);
	free(data->data);
	return 0;
}
