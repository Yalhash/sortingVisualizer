#include <stdint.h>
#include <stdexcept>
#include <string>
#include <iostream>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

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




//pixel struct for storing pixel data as well as position in picture
struct Pixel {
	unsigned char r;
	unsigned char g;
	unsigned char b;
	int position;
};

//misc functions
Pixel* getOrderedPixelFromRBG(uint8_t*, int);
uint8_t* getRGBFromOrderedPixel(Pixel*, int);
void updateRGB(Pixel*, uint8_t*, int);
void updateSingleRGB(Pixel*, uint8_t*, int);
void printPixels(Pixel*, int);
void printRGB(unsigned char*, int);
void swap(Pixel*, uint8_t*, int, int, int, VideoCapture*);
void swapNoFrame(Pixel*, uint8_t*, int, int, int);
void delay(uint8_t*, int, VideoCapture*);
void shufflePixels(Pixel*, uint8_t*, int, VideoCapture*);
void shuffleNoVid(Pixel*, uint8_t*, int);
void reverseInPlace(Pixel*, uint8_t*, int, VideoCapture*);
int partition(Pixel*, uint8_t*, int, VideoCapture*, int, int);
void merge(Pixel*, uint8_t*, int, int, int, int, VideoCapture*);
//sorts:

void quickSort(Pixel*, uint8_t*, int, VideoCapture*, int low = 0, int high = -1);
void mergeSort(Pixel*, uint8_t*, int, VideoCapture*, int left = 0, int right = -1);
void bubbleSort(Pixel*, uint8_t*, int, VideoCapture*);
void heapSort(Pixel*, uint8_t*, int, VideoCapture*);
void heapSortMin(Pixel*, uint8_t*, int, VideoCapture*);
void countingSort(Pixel*, uint8_t*, int, VideoCapture*);
void radixSortBaseTen(Pixel*, uint8_t*, int, VideoCapture*);


//global variables
unsigned int FRAMECOUNT = 0;
unsigned int SKIP = 100;
char LOADSIGN = '\\';
int main() {
	const char *EXT = "mpeg1video";
	char FILENAME[] = "visualized_sort.mpg";
	int width, height, bpp;
	const char *IMAGEFILE; //"../assets/testIMGBig.PNG"
	uint8_t* rgb_image = NULL;
	std::vector <std::string> actionList;
	std::string inputStr;
	std::cout << "-------------------------------------------------------------------------------------" << std::endl;
	std::cout << "------------------------------ Image Sorting Visualizer -----------------------------" << std::endl;
	std::cout << "-------------------------------------------------------------------------------------" << std::endl;
	std::cout << "if you need help, type \"help\" for usage, or \"exit\" to exit" << std::endl;
	


	
	//main loop
	while (true) {
		std::cout << ">> ";
		std::getline(std::cin, inputStr);
		if (inputStr == "help") {
			std::cout << "-------------------------------------------------------------------------------------" << std::endl;
			std::cout << "Overview:\n    Choose an image file with file command, and use the add command to add sorts,\n    delays, or scrambles in any order" << std::endl;
			std::cout << "-------------------------------------------------------------------------------------" << std::endl;
			std::cout << "Commands:\n    file <filename>, Usage: choose the file to make a visualization out of." << std::endl;
			std::cout << "    add <action>, Usage: add an action to the visualization." << std::endl;
			std::cout << "    clear, Usage: clears the list of actions added prior." << std::endl;
			std::cout << "    status, Usage: view the list of actions added prior." << std::endl;
			std::cout << "    create, Usage: creates the visualization, as long as there is at least one action,\n                   and a valid file. (Exits upon completion)" << std::endl;
			std::cout << "-------------------------------------------------------------------------------------" << std::endl;
			std::cout << "Actions:\n    Sorts: bubble, quick, merge, heapMax, heapMin, counting, radix" << std::endl;
			std::cout << "    Other: delay (1 second), shuffle, shuffleNoVid, reverse" << std::endl;
		}
		else if (inputStr.find("file") != std::string::npos) {
			//read the filename, and try to open it
			std::string imageFileInput;
			
			if(inputStr.size() > 4){
				imageFileInput = inputStr.substr(5);
			}
			else {
				std::cout << ">> provide a file: ";
				std::getline(std::cin, imageFileInput);
			}
			IMAGEFILE = imageFileInput.c_str();
			try {
				rgb_image = stbi_load(IMAGEFILE, &width, &height, &bpp, 3);
			}
			catch (const std::exception& e) {
				std::cout << ">> Couldn't read file." << std::endl;
			}

			if (!rgb_image) {
				std::cout << ">> Couldn't find file." << std::endl;
			}
			else {
				std::cout << ">> " << imageFileInput << " successfully loaded." << std::endl;
			}


		}
		else if (inputStr.find("add") != std::string::npos) {
			//add stuff to the actionList
			std::string actionInput;
			if (inputStr.size() > 3) {
				actionInput = inputStr.substr(4);
			}
			else {
				std::cout << ">> provide an action: ";
				std::getline(std::cin, actionInput);
			}

			if (actionInput == "bubble" || actionInput == "quick" || actionInput == "merge" || actionInput == "heapMax" || actionInput == "heapMin" || actionInput == "counting" || actionInput == "radix" || actionInput == "shuffle" || actionInput == "shuffleNoVid" || actionInput == "reverse"|| actionInput == "delay") {
				actionList.push_back(actionInput);
				std::cout << ">> " << actionInput << " successfully added." << std::endl;
			}
			else {
				std::cout << ">> Invalid action!" << std::endl;
			}
			

			
		}
		else if (inputStr == "status") {
			std::cout << "-------------------------------------------------------------------------------------" << std::endl;
			if (actionList.size() == 0) {
				std::cout << "Currently no actions." << std::endl;
			}
			else {
				std::cout << "Actions: " << std::endl;
				for (int i = 0; i < actionList.size(); i++) {
					std::cout << "    " << i + 1 << ". " << actionList[i] << std::endl;
				}
			}
			std::cout << "-------------------------------------------------------------------------------------" << std::endl;
		}
		else if (inputStr == "clear") {
			actionList.clear();
			std::cout << ">> Actions cleared." << std::endl;
		}
		else if (inputStr == "create") {
			if (actionList.size() < 1) {
				std::cout << ">> Need at least 1 action." << std::endl;
			}
			else if(!rgb_image){
				std::cout << ">> Need a valid file." << std::endl;
			}
			else {
				int size = width * height;
				int fps, bitrate;
				fps = 60; //480 by default
				bitrate = 3000;
				SKIP = width + height;
				Pixel* pixelArray = getOrderedPixelFromRBG(rgb_image, size);
				VideoCapture *capture = Init(width, height, fps, bitrate);

				for (int i = 0; i < actionList.size(); i++) {
					if (actionList[i] == "bubble") {
						SKIP *= 5;
						bubbleSort(pixelArray, rgb_image, size, capture);
						SKIP = SKIP / 5;
					}
					else if(actionList[i] == "quick") {
						quickSort(pixelArray, rgb_image, size, capture);
					}
					else if (actionList[i] == "merge") {
						mergeSort(pixelArray, rgb_image, size, capture);
					}
					else if (actionList[i] == "heapMax") {
						heapSort(pixelArray, rgb_image, size, capture);
					}
					else if (actionList[i] == "heapMin") {
						heapSortMin(pixelArray, rgb_image, size, capture);
					}
					else if (actionList[i] == "counting") {
						countingSort(pixelArray, rgb_image, size, capture);
					}
					else if (actionList[i] == "radix") {
						radixSortBaseTen(pixelArray, rgb_image, size, capture);
					}
					else if (actionList[i] == "shuffle") {
						shufflePixels(pixelArray, rgb_image, size, capture);
					}
					else if (actionList[i] == "shuffleNoVid") {
						shuffleNoVid(pixelArray, rgb_image, size);
					}
					else if (actionList[i] == "reverse") {
						reverseInPlace(pixelArray, rgb_image, size, capture);
					}
					else if (actionList[i] == "delay") {
						delay(rgb_image, fps, capture);
					}
				}
				capture->Finish();
				stbi_image_free(rgb_image);
				delete[] pixelArray;
				pixelArray = NULL;
				return 0;
			}
		}
		else if (inputStr == "exit") {
			return 0;
		}
		else {
			std::cout << ">> Invalid command!" << std::endl;
		}		
	}
	return 0;
}

/*--------------------------------------------------------------------------------------------------------------------------------------------------*/
/*----------------------------------------------------------FUNCTIONS-------------------------------------------------------------------------------*/
/*--------------------------------------------------------------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------UPDATE FUNCTIONS-------------------------------------------------------*/
Pixel* getOrderedPixelFromRBG(uint8_t* rgb ,int size) {
	Pixel* newArray;
	newArray = new Pixel[size];
	//width & height are amount of pixels, not amount of elements
	//thus there are 3*width*height actual elements in the rgb array
	for (int i = 0; i < size; i++) {
		newArray[i] = Pixel();
		newArray[i].r = rgb[3 * i];
		newArray[i].g = rgb[3 * i + 1];
		newArray[i].b = rgb[3 * i + 2];
		newArray[i].position = i;
	}
	return newArray;
}

uint8_t* getRGBFromOrderedPixel(Pixel* pixelArr, int size){
	unsigned char* newArray;
	newArray = new unsigned char[size * 3];
	for (int i = 0; i < size; i++) {
		newArray[i * 3] = pixelArr[i].r;
		newArray[i * 3+1] = pixelArr[i].g;
		newArray[i * 3+2] = pixelArr[i].b;
	}
	return newArray;
}


/*----------------------------------------------------------UPDATE FUNCTIONS-------------------------------------------------------*/
inline void updateVisual() {
	switch (FRAMECOUNT % 1000) {
	case 0: LOADSIGN = '\\';
		break;
	case 250: LOADSIGN = '|';
		break;
	case 500: LOADSIGN = '/';
		break;
	case 750: LOADSIGN = '-';
		break;
	}
	std::cout << "\r" << LOADSIGN << "Generating Video, Frame: " << FRAMECOUNT++ << LOADSIGN << "\r";
}
//this function is not really used, as it is more efficient to 
//just update the pixels as needed, instead of the entire photo
void updateRGB(Pixel* pixelArr, uint8_t* RGB, int size) {
	for (int i = 0; i < size; i++) {
		RGB[i * 3] = pixelArr[i].r;
		RGB[i * 3 + 1] = pixelArr[i].g;
		RGB[i * 3 + 2] = pixelArr[i].b;
	}
	
}

//changes a single pixel inside the pixel array to be the same as a new pixel
void updatePixel(Pixel* pixelArr, uint8_t* rgb, Pixel newPix, int index, int size, VideoCapture* capture) {
	pixelArr[index].r = newPix.r;
	pixelArr[index].g = newPix.g;
	pixelArr[index].b = newPix.b;
	pixelArr[index].position = newPix.position;

	updateVisual();


	updateSingleRGB(pixelArr, rgb, index);
	if (FRAMECOUNT%SKIP == 0) {
		capture->AddFrame(rgb);
	}
	
}

void updateSingleRGB(Pixel* pixelArr, uint8_t* RGB, int index) {
	RGB[index * 3] = pixelArr[index].r;
	RGB[index * 3 + 1] = pixelArr[index].g;
	RGB[index * 3 + 2] = pixelArr[index].b;
}

void copyPixelArray(Pixel* pixelArr, Pixel* newArr, int size) {
	for (int i = 0; i < size; i++) {
		newArr[i].r = pixelArr[i].r;
		newArr[i].g = pixelArr[i].g;
		newArr[i].b = pixelArr[i].b;
		newArr[i].position = pixelArr[i].position;
	}
}

/*---------------------------------------------------------------DEBUG PRINTS-------------------------------------------------------*/

void printPixels(Pixel* pixelArr, int size){
	for (int i = 0; i < size; i++) {
		printf("%d: (%X, %X, %X)\n", pixelArr[i].position, pixelArr[i].r, pixelArr[i].g, pixelArr[i].b);
	}
}

void printRGB(unsigned char* rgbArr, int size) {
	for (int i = 0; i < size; i++) {
		printf("(%X, %X, %X)\n", rgbArr[i * 3], rgbArr[i * 3 + 1], rgbArr[i * 3 + 2]);
	}
}


/*--------------------------------------------------------------------shuffle, swap, and delays-----------------------------------------------------------------*/

//used to randomize the pixels in a visual way, each swap is captured and added to the video
void shufflePixels(Pixel* pixelArr, uint8_t* rgb, int size, VideoCapture* capture){
	srand(time(0));
	int randIndex;
	for (int i = 0; i < size; i++) {
		//when using larger images, the rand function will not
		//produce values up to the size, so a larger number is needed
		randIndex = (rand()*rand()) % size;
		std::cout << "shuffle ";
		swap(pixelArr, rgb,  i, randIndex, size, capture);
	}

}
//used to start a sort shuffled, or instantly shuffle (in terms of the video
void shuffleNoVid(Pixel* pixelArr, uint8_t* rgb, int size) {
	srand(time(0));
	int randIndex;
	for (int i = 0; i < size; i++) {
		randIndex = rand() % size;
		swapNoFrame(pixelArr, rgb, i, randIndex, size);
	}

}

//used for swapping pixels without creating a frame
void swapNoFrame(Pixel* pixelArr, uint8_t* rgb, int index1, int index2, int size) {
	Pixel tempPixel = pixelArr[index1];
	pixelArr[index1] = pixelArr[index2];
	pixelArr[index2] = tempPixel;
	//capture the image at this moment
	updateVisual();

	updateSingleRGB(pixelArr, rgb, index1);
	updateSingleRGB(pixelArr, rgb, index2);
}

void swap(Pixel* pixelArr, uint8_t* rgb, int index1, int index2, int size, VideoCapture* capture) {
	Pixel tempPixel = pixelArr[index1];
	pixelArr[index1] = pixelArr[index2];
	pixelArr[index2] = tempPixel;
	//capture the image at this moment
	updateVisual();
	updateSingleRGB(pixelArr, rgb, index1);
	updateSingleRGB(pixelArr, rgb, index2);
	if (FRAMECOUNT%SKIP == 0) {
		capture->AddFrame(rgb);
	}

}


//add still frames to the video of amount frames
void delay(uint8_t* rgb, int frames, VideoCapture* capture) {

	for (int i = 0; i < frames; i++) {
		updateVisual();
		capture->AddFrame(rgb);
		
	}
}



/*----------------------------------------------------------------------SORTS--------------------------------------------------------------------------*/
	

//bubble sort
void bubbleSort(Pixel* pixelArr, uint8_t* rgb, int size, VideoCapture* capture){
	bool noSwap;
	for (int i = 0; i < size; i++) {
		noSwap = true;
		for (int j = 0; j < size-1; j++) {
			if (pixelArr[j].position > pixelArr[j + 1].position) {
				std::cout << "bubble ";
				swap(pixelArr, rgb, j, j + 1, size, capture);
				noSwap = false;
			}
		}
		if (noSwap) {
			break;
		}
	}
}


//merge sort

void merge(Pixel* pixelArr, uint8_t* rgb, int size, int left, int mid, int right, VideoCapture* capture) {
	int i, j, k;
	int leftSize = mid - left + 1;
	int rightSize = right - mid;
	//make temporary arrays
	Pixel *L, *R;
	L = new Pixel[leftSize];
	R = new Pixel[rightSize];

	for (i = 0; i < leftSize; i++)
		L[i] = pixelArr[left + i];
	for (j = 0; j < rightSize; j++)
		R[j] = pixelArr[mid + 1 + j];

	i = 0;
	j = 0;
	k = left;
	//k = current value in list (left-most non sorted value)

	while (i < leftSize && j < rightSize) {
		if (L[i].position <= R[j].position) {
			std::cout << "merge ";
			updatePixel(pixelArr, rgb, L[i], k, size, capture);
			i++;
		}
		else {
			std::cout << "merge ";
			updatePixel(pixelArr, rgb, R[j], k, size, capture);
			j++;
		}
		k++;
	}

	//copy remaining 
	while (i < leftSize) {
		std::cout << "merge ";
		updatePixel(pixelArr, rgb, L[i], k, size, capture);
		i++;
		k++;
	}
	while (j < rightSize) {
		std::cout << "merge ";
		updatePixel(pixelArr, rgb, R[j], k, size, capture);
		j++;
		k++;
	}
	//try to make sure no memory leaks
	delete[] L;
	L = NULL;
	delete[] R;
	R = NULL;
}

void mergeSort(Pixel* pixelArr, uint8_t* rgb, int size, VideoCapture* capture, int left, int right) {

	if (right == -1) {	//for first entry
		right = size - 1; 
	}
	
	if (left < right) {
		//find midpoint
		int mid = left + (right - left) / 2;
		//sort the left and right
		mergeSort(pixelArr, rgb, size, capture, left, mid);
		mergeSort(pixelArr, rgb, size, capture, mid + 1, right);
		//merge the halves
		merge(pixelArr, rgb, size, left, mid, right, capture);
	}
}



//quick sort 

//takes last element as partition
int partition(Pixel* pixelArr, uint8_t* rgb, int size, VideoCapture* capture, int low, int high) {
	Pixel pivot = pixelArr[high];
	int leftInd = low - 1;

	for (int i = low; i <= high - 1; i++) {
		if (pixelArr[i].position <= pivot.position) {
			leftInd++;
			std::cout << "quick ";
			swap(pixelArr, rgb, leftInd, i, size, capture);
		}
	}
	//take the partition from the end of the list, and centre it
	std::cout << "quick ";
	swap(pixelArr, rgb, leftInd + 1, high, size, capture);
	return (leftInd + 1);
}

void quickSort(Pixel* pixelArr, uint8_t* rgb, int size, VideoCapture* capture, int low, int high) {
	
	if (high == -1) { //for first entry
		high = size - 1;
	}

	if (low < high) {
		int partitionInd = partition(pixelArr, rgb, size, capture, low, high);
		quickSort(pixelArr, rgb, size, capture, low, partitionInd-1); //left of part
		quickSort(pixelArr, rgb, size, capture, partitionInd + 1, high); //right of part
	}
}

//heap sort

int getLeftChild(int index) { return index * 2 + 1; }
int getRightChild(int index) { return index * 2 + 2; }
bool hasLeftChild(int index, int size) {
	return getLeftChild(index) < size;
}
bool hasRightChild(int index, int size) {
	return getRightChild(index) < size;
}



void siftDown(Pixel* pixelArr, uint8_t* rgb, int size, int currentRoot, VideoCapture* capture) {
	int largestIndex = currentRoot;

	if (hasLeftChild(currentRoot, size)) {
		//check if left is larger than root
		if (pixelArr[getLeftChild(currentRoot)].position > pixelArr[currentRoot].position) {
			largestIndex = getLeftChild(currentRoot);
		}
		//no need to check for right child if no left
		if (hasRightChild(currentRoot, size) && pixelArr[getRightChild(currentRoot)].position > pixelArr[largestIndex].position) {
			largestIndex = getRightChild(currentRoot);
		}
	}


	if (currentRoot != largestIndex) {
		std::cout << "heap max ";
		swap(pixelArr, rgb, currentRoot, largestIndex, size, capture);
		siftDown(pixelArr, rgb, size, largestIndex, capture);//repeat until no swaps are needed
	}
}

void heapify(Pixel* pixelArr, uint8_t* rgb, int size, VideoCapture* capture) {
	//start at 2nd last row and move up
	for (int i = size / 2 - 1; i >= 0; i--) {
		siftDown(pixelArr,rgb, size, i, capture);
	}
}

void heapSort(Pixel* pixelArr, uint8_t* rgb, int size, VideoCapture* capture) {

	heapify(pixelArr, rgb, size, capture);
	for (int i = size - 1; i >= 0; i--)
	{
		// move root to end
		std::cout << "heap max ";
		swap(pixelArr, rgb, 0, i, size, capture);
		
		// call recreate the heap
		siftDown(pixelArr, rgb, i, 0, capture);
	}
}




//minimum heap sort

void reverseInPlace(Pixel* pixelArr, uint8_t* rgb, int size, VideoCapture* capture) {
	for (int i = 0; i < size/2; i++) {
		std::cout << "reverse ";
		swap(pixelArr, rgb, i, size - i-1, size, capture);
	}
}


void siftDownMin(Pixel* pixelArr, uint8_t* rgb, int size, int currentRoot, VideoCapture* capture) {
	int smallestIndex = currentRoot;

	if (hasLeftChild(currentRoot, size)) {
		//check if left is smaller than root
		if (pixelArr[getLeftChild(currentRoot)].position < pixelArr[currentRoot].position) {
			smallestIndex = getLeftChild(currentRoot);
		}
		//no need to check for right child if no left
		if (hasRightChild(currentRoot, size) && pixelArr[getRightChild(currentRoot)].position < pixelArr[smallestIndex].position) {
			smallestIndex = getRightChild(currentRoot);
		}
	}

	if (currentRoot != smallestIndex) {
		std::cout << "heap min ";
		swap(pixelArr, rgb, currentRoot, smallestIndex, size, capture);
		siftDownMin(pixelArr, rgb, size, smallestIndex, capture);//repeat until no swaps are needed
	}
}




void heapifyMin(Pixel* pixelArr, uint8_t* rgb, int size, VideoCapture* capture){
	//start at 2nd last row and move up
	for (int i = size / 2 - 1; i >= 0; i--) {
		siftDownMin(pixelArr, rgb, size, i, capture);
	}

}


void heapSortMin(Pixel* pixelArr, uint8_t* rgb, int size, VideoCapture* capture){
	heapifyMin(pixelArr, rgb, size, capture);
	for (int i = size - 1; i >= 0; i--)
	{
		std::cout << "heap min ";
		// move root to end
		swap(pixelArr, rgb, 0, i, size, capture);

		// call recreate the heap
		siftDownMin(pixelArr, rgb, i, 0, capture);
	}
	reverseInPlace(pixelArr, rgb, size, capture);
}



//counting sort


void countingSort(Pixel* pixelArr, uint8_t* rgb, int size, VideoCapture* capture) {
	int* countArr;
	countArr = new int[size];
	Pixel* newArr;
	newArr = new Pixel[size];
	copyPixelArray(pixelArr, newArr, size);


	//create the count array
	for (int i = 0; i < size; i++) {
		countArr[i] = 0;
	}
	for (int i = 0; i < size; i++) {
		countArr[pixelArr[i].position] += 1;
	}

	//modify the count array to the cumulative version
	for (int i = 1; i < size; i++) {
		countArr[i] += countArr[i - 1];
	}


	//go through the last Array backwards and create sorted array
	for (int i = size - 1; i >= 0; i--) {
		countArr[newArr[i].position]--;
		std::cout << "count ";
		updatePixel(pixelArr, rgb, newArr[i], countArr[newArr[i].position], size, capture);
	}

	//delete temp arrays
	delete[] newArr;
	delete[] countArr;
	countArr = NULL;
	newArr = NULL;
}



//radix base 10

int powTen(int n) {
	int power = 1;
	for (int i = 0; i < n; i++) {
		power *= 10;
	}
	return power;
}

int getDigit(int num, int digitIndex) {
	int finalNum = num;
	finalNum = finalNum / powTen(digitIndex);
	return finalNum % 10;
}

int getNumDigits(int num) {
	int i = 0;
	while (num >= powTen(i)) {
		i++;
	}
	if (i == 0) {
		return 1;
	}
	return i;
}

void countingSortRadix(Pixel* pixelArr, uint8_t* rgb, int size, int range, int digit, VideoCapture* capture) {
	int* countArr;
	Pixel* newArr;
	newArr = new Pixel[size];
	copyPixelArray(pixelArr, newArr, size);
	countArr = new int[range];

	//create the count array
	for (int i = 0; i < range; i++) {
		countArr[i] = 0;
	}
	for (int i = 0; i < size; i++) {
		countArr[getDigit(pixelArr[i].position, digit)] += 1;
	}

	//modify the count array to the cumulative version
	for (int i = 1; i < range; i++) {
		countArr[i] += countArr[i - 1];
	}

	//go through the last Array backwards and create sorted array
	for (int i = size - 1; i >= 0; i--) {
		countArr[getDigit(newArr[i].position, digit)]--;
		std::cout << "radix ";
		updatePixel(pixelArr, rgb, newArr[i], countArr[getDigit(newArr[i].position, digit)], size, capture);
	}


	//delete temp arrays
	delete[] newArr;
	delete[] countArr;
	countArr = NULL;
	newArr = NULL;
}

//(Pixel* pixelArr, uint8_t* rgb, int size, VideoCapture* capture)
void radixSortBaseTen(Pixel* pixelArr, uint8_t* rgb, int size, VideoCapture* capture) {
	int range = getNumDigits(size);
	for (int i = 0; i < range; i++) {
		countingSortRadix(pixelArr, rgb, size, 10, i, capture);
	}
}



