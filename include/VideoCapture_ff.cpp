#include "VideoCapture.h"
#define VIDEO_TMP_FILE "tmp.h264"
#define FINAL_FILE_NAME "sortingSample.mp4"



void VideoCapture::Init(int width, int height, int fpsrate, int bitrate) {

	fps = fpsrate;

	int err;

	//get format from file name (given mp4, h264, ect...)
	if (!(oformat = av_guess_format(NULL, VIDEO_TMP_FILE, NULL))) {
		Debug("Failed to define output format", 0);
		return;
	}

	//allocate space for the context (needs to be done dynamically depending on format)
	if ((err = avformat_alloc_output_context2(&ofctx, oformat, NULL, VIDEO_TMP_FILE) < 0)) {
		Debug("Failed to allocate output context", err);
		Free();
		return;
	}

	//find an encoder based off of the format codec
	if (!(codec = avcodec_find_encoder(oformat->video_codec))) {
		Debug("Failed to find encoder", 0);
		Free();
		return;
	}

	//create a new stream based on the format context as well as the codec
	if (!(videoStream = avformat_new_stream(ofctx, codec))) {
		Debug("Failed to create new stream", 0);
		Free();
		return;
	}

	//allocate context for the codec (needs to be done dynamically, same reason as above)
	if (!(cctx = avcodec_alloc_context3(codec))) {
		Debug("Failed to allocate codec context", 0);
		Free();
		return;
	}


	//setting parameters on the codec parameters for the stream
	videoStream->codecpar->codec_id = oformat->video_codec;
	videoStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
	videoStream->codecpar->width = width;
	videoStream->codecpar->height = height;
	videoStream->codecpar->format = AV_PIX_FMT_YUV420P;
	videoStream->codecpar->bit_rate = bitrate * 1000;
	videoStream->time_base = { 1, fps };

	//transfering stream parameters to the codec context (and some more below)
	avcodec_parameters_to_context(cctx, videoStream->codecpar);
	cctx->time_base = { 1, fps };
	cctx->max_b_frames = 2;
	cctx->gop_size = 12;

	//set presets
	if (videoStream->codecpar->codec_id == AV_CODEC_ID_H264) {
		av_opt_set(cctx, "preset", "ultrafast", 0);
	}

	//checking if and setting the global header flag
	if (ofctx->oformat->flags & AVFMT_GLOBALHEADER) {
		cctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	//updating the codec parameters of the video stream based on the codec context
	avcodec_parameters_from_context(videoStream->codecpar, cctx);

	//opening the codec
	if ((err = avcodec_open2(cctx, codec, NULL)) < 0) {
		Debug("Failed to open codec", err);
		Free();
		return;
	}

	//opening the file for 
	if (!(oformat->flags & AVFMT_NOFILE)) {
		if ((err = avio_open(&ofctx->pb, VIDEO_TMP_FILE, AVIO_FLAG_WRITE)) < 0) {
			Debug("Failed to open file", err);
			Free();
			return;
		}
	}

	//writing header to the file
	if ((err = avformat_write_header(ofctx, NULL)) < 0) {
		Debug("Failed to write header", err);
		Free();
		return;
	}

	//printing format info into the file
	av_dump_format(ofctx, 0, VIDEO_TMP_FILE, 1);
}

void VideoCapture::AddFrame(uint8_t *data) {
	int err;

	//create the video frame if its the first frame
	if (!videoFrame) {
		videoFrame = av_frame_alloc();
		videoFrame->format = AV_PIX_FMT_YUV420P;
		videoFrame->width = cctx->width;
		videoFrame->height = cctx->height;

		if ((err = av_frame_get_buffer(videoFrame, 32)) < 0) {
			Debug("Failed to allocate picture", err);
			return;
		}
	}

	//set up for scaling
	if (!swsCtx) {
		swsCtx = sws_getContext(cctx->width, cctx->height, AV_PIX_FMT_RGB24, cctx->width, cctx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, 0, 0, 0);
	}

	//setting the linesize to be 3x the width (RGB, 3 elements per pixel?)
	int inLinesize[1] = { 3 * cctx->width };

	//resizing the next frame
	sws_scale(swsCtx, (const uint8_t * const *)&data, inLinesize, 0, cctx->height, videoFrame->data, videoFrame->linesize);

	//setting thee next frame
	videoFrame->pts = frameCounter++;

	//sending the frame to the codec
	if ((err = avcodec_send_frame(cctx, videoFrame)) < 0) {
		Debug("Failed to send frame", err);
		return;
	}

	//create a packet for recieving output from the codec
	AVPacket pkt;
	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;

	//recieve the packet and write the frame then free the packet
	if (avcodec_receive_packet(cctx, &pkt) == 0) {
		pkt.flags |= AV_PKT_FLAG_KEY;
		av_interleaved_write_frame(ofctx, &pkt);
		av_packet_unref(&pkt);
	}
}

void VideoCapture::Finish() {
	//DELAYED FRAMES
	AVPacket pkt;
	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;

	//recieve all packets until a null packet
	for (;;) {
		avcodec_send_frame(cctx, NULL);
		if (avcodec_receive_packet(cctx, &pkt) == 0) {
			av_interleaved_write_frame(ofctx, &pkt);
			av_packet_unref(&pkt);
		}
		else {
			break;
		}
	}

	//write the trailing stuff to the file
	av_write_trailer(ofctx);
	if (!(oformat->flags & AVFMT_NOFILE)) {
		int err = avio_close(ofctx->pb);
		if (err < 0) {
			Debug("Failed to close file", err);
		}
	}

	//free all of the stuff from before
	Free();

	Remux();
}

void VideoCapture::Free() {
	if (videoFrame) {
		av_frame_free(&videoFrame);
	}
	if (cctx) {
		avcodec_free_context(&cctx);
	}
	if (ofctx) {
		avformat_free_context(ofctx);
	}
	if (swsCtx) {
		sws_freeContext(swsCtx);
	}
}

void VideoCapture::Remux() {
	AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
	int err;

	//open input from the file we just wrote to (the YUV/h264 one)
	if ((err = avformat_open_input(&ifmt_ctx, VIDEO_TMP_FILE, 0, 0)) < 0) {
		Debug("Failed to open input file for remuxing", err);
		if (ifmt_ctx) {
			avformat_close_input(&ifmt_ctx);
		}
		if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
			avio_closep(&ofmt_ctx->pb);
		}
		if (ofmt_ctx) {
			avformat_free_context(ofmt_ctx);
		}
	}

	//get stream info for the context
	if ((err = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
		Debug("Failed to retrieve input stream information", err);
		if (ifmt_ctx) {
			avformat_close_input(&ifmt_ctx);
		}
		if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
			avio_closep(&ofmt_ctx->pb);
		}
		if (ofmt_ctx) {
			avformat_free_context(ofmt_ctx);
		}
	}

	//open output context for the final file
	if ((err = avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, FINAL_FILE_NAME))) {
		Debug("Failed to allocate output context", err);
		if (ifmt_ctx) {
			avformat_close_input(&ifmt_ctx);
		}
		if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
			avio_closep(&ofmt_ctx->pb);
		}
		if (ofmt_ctx) {
			avformat_free_context(ofmt_ctx);
		}
	}

	//make two streams (one from the input and one for the context)
	AVStream *inVideoStream = ifmt_ctx->streams[0];
	AVStream *outVideoStream = avformat_new_stream(ofmt_ctx, NULL);
	if (!outVideoStream) {
		Debug("Failed to allocate output video stream", 0);
		if (ifmt_ctx) {
			avformat_close_input(&ifmt_ctx);
		}
		if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
			avio_closep(&ofmt_ctx->pb);
		}
		if (ofmt_ctx) {
			avformat_free_context(ofmt_ctx);
		}
	}

	//set the parameters for the output stream
	outVideoStream->time_base = { 1, fps };
	avcodec_parameters_copy(outVideoStream->codecpar, inVideoStream->codecpar);
	outVideoStream->codecpar->codec_tag = 0;

	if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
		if ((err = avio_open(&ofmt_ctx->pb, FINAL_FILE_NAME, AVIO_FLAG_WRITE)) < 0) {
			Debug("Failed to open output file", err);
			if (ifmt_ctx) {
				avformat_close_input(&ifmt_ctx);
			}
			if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
				avio_closep(&ofmt_ctx->pb);
			}
			if (ofmt_ctx) {
				avformat_free_context(ofmt_ctx);
			}
		}
	}

	//write the header
	if ((err = avformat_write_header(ofmt_ctx, 0)) < 0) {
		Debug("Failed to write header to output file", err);
		if (ifmt_ctx) {
			avformat_close_input(&ifmt_ctx);
		}
		if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
			avio_closep(&ofmt_ctx->pb);
		}
		if (ofmt_ctx) {
			avformat_free_context(ofmt_ctx);
		}
	}

	AVPacket videoPkt;
	int ts = 0;
	while (true) {
		//reading the frame from input
		if ((err = av_read_frame(ifmt_ctx, &videoPkt)) < 0) {
			break;
		}

		//setting packet stuff
		videoPkt.stream_index = outVideoStream->index;
		videoPkt.pts = ts;
		videoPkt.dts = ts;
		videoPkt.duration = av_rescale_q(videoPkt.duration, inVideoStream->time_base, outVideoStream->time_base);
		ts += videoPkt.duration;
		videoPkt.pos = -1;

		//write the packet to the output file
		if ((err = av_interleaved_write_frame(ofmt_ctx, &videoPkt)) < 0) {
			Debug("Failed to mux packet", err);
			av_packet_unref(&videoPkt);
			break;
		}
		av_packet_unref(&videoPkt);
	}

	av_write_trailer(ofmt_ctx);


}