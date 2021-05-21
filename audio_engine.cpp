#include "audio_engine.h"

int AudioCapture::audioInit(int channel_layout, AVSampleFormat format, int samples)
{
	error[128] = { 0 };
	av_register_all();
	avdevice_register_all();
	av_log_set_level(AV_LOG_DEBUG);
	packet_ = av_packet_alloc();
	av_init_packet(packet_);
	int ret = createFrame(channel_layout, format, samples);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "create frame failure.\n");
		return -1;
	}
	ret = audioOpenDevice();
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "create frame failure.\n");
		return -1;
	}
	return 0;
}
void AudioCapture::audioDeinit()
{
	audioCloseDevice();
	destoryFrame();
	av_packet_free(&packet_);
}
void AudioCapture::destoryFrame()
{
	av_frame_unref(frame_);
}
int AudioCapture::audioCloseDevice()
{
	if (fmtCtx_)
	{
		avformat_close_input(&fmtCtx_);
	}

	return 0;
}
int AudioCapture::createFrame(int channel_layout, AVSampleFormat format, int nb_samples)
{
	frame_ = av_frame_alloc();
	frame_->channel_layout = channel_layout;
	frame_->format = format;
	frame_->nb_samples = nb_samples;
	int ret = av_frame_get_buffer(frame_, 0);
	if (ret != 0){
		av_strerror(ret, error, 128);
		av_log(NULL, AV_LOG_ERROR, "open input failure.[%d][%s]\n", AVERROR(ret), error);
		return -1;
	}
	printf("channel_layout:%d format:%d nb_samples:%d frame_->line_size:%d",
		channel_layout, format, nb_samples, frame_->linesize[0]);
	return 0;
}

int AudioCapture::audioOpenDevice()
{
	AVInputFormat *inputFmt = av_find_input_format(libName_.c_str());
	if (!inputFmt) {
		av_log(NULL, AV_LOG_ERROR, "find lib %s failure.\n", libName_.c_str());
		return -1;
	}

	int ret = avformat_open_input(&fmtCtx_, deviceName_.c_str(), inputFmt, NULL);
	if (ret != 0) {
		av_strerror(ret, error, 128);
		av_log(NULL, AV_LOG_ERROR, "open input failure.[ret][%s]\n", AVERROR(ret), error);
		return -1;
	}
	av_dump_format(fmtCtx_, 0, deviceName_.c_str(), 0);
	return 0;
}

int AudioCapture::audioCaptureFrame(AVFrame **frame)
{
	static int read_size = -1;
	static int write_size = 0;
	//最后一片是2184, 这里就只有第一次读的时候，会进 while
	while(read_size < 0) {
		int ret = av_read_frame(fmtCtx_, packet_);
		if (ret != 0) {
			return ret;
		}
		printf("read packet.size = %d, read_size = %d\n", packet_->size, read_size);
		//av_packet_unref(packet_);
		read_size = packet_->size;
		write_size = 0;
	} 
	if (read_size < 4096) { //最后一片是 2184
		memcpy(frame_->data[0], packet_->data + write_size, read_size);
		av_read_frame(fmtCtx_, packet_); //不够一帧时，读取 packet 数据，继续填充 frame->data[0] ，长度是 4096 - read_size
		printf("last read_size = %d\n", read_size);
		int remain_len = 4096 - read_size;
		memcpy(frame_->data[0] + read_size, packet_->data, remain_len);
		read_size = packet_->size - remain_len; //重置新 packet 的 read_size 和 write_size
		write_size = remain_len;
	} else {
		memcpy(frame_->data[0], packet_->data + write_size, 4096);
		read_size -= 4096;
		write_size += 4096;
	}
	
	//frame_->data[0]的大小是参数控制的，存放的是一帧的数据量，
	//frame_->linesize[0]就是其大小，
    // pkt.size = 88200，包括了好几帧的数据，直接往data[0]里拷贝会越界.
	*frame = frame_;
	return 0;
}




/////////////////////////// AudioSample 重采样类实现///////////////////////////////////////////////////////////////
AudioSample::~AudioSample()
{
	if (srcData_)
		av_freep(&srcData_[0]);
	av_freep(srcData_);
	if (dstData_)
		av_freep(&dstData_[0]);
	av_freep(dstData_);
	swr_free(&swrCtx_);
}

int AudioSample::audioSampleInit()
{
	swrCtx_ = swr_alloc_set_opts(NULL,
		dstChLayout_, dstFormat_, dstRate_,
		srcChLayout_, srcFormat_, srcRate_,
		0, NULL);
	if (!swrCtx_) {
		av_log(NULL, AV_LOG_ERROR, "create swr ctx fail.\n");
		return -1;
	}
	swr_init(swrCtx_);
	audioSampleCreateData();
	
	return 0;
}

AVBufferRef* myav_buffer_alloc(int size)
{
	AVBufferRef* ret = NULL;
	uint8_t* data = NULL;

	data = (uint8_t*)av_malloc(size);
	if (!data)
		return NULL;

	ret = av_buffer_create(data, size, av_buffer_default_free, NULL, 0);
	if (!ret)
		av_freep(&data);

	return ret;
}
int my_get_audio_buffer(AVFrame* frame, int align)
{
	int channels;
	int planar = av_sample_fmt_is_planar((AVSampleFormat)frame->format);
	int planes;
	int ret, i;

	if (!frame->channels)
		frame->channels = av_get_channel_layout_nb_channels(frame->channel_layout);

	channels = frame->channels;
	planes = planar ? channels : 1;

	//CHECK_CHANNELS_CONSISTENCY(frame);
	if (!frame->linesize[0]) {
		ret = av_samples_get_buffer_size(&frame->linesize[0], channels,
			frame->nb_samples, (AVSampleFormat)frame->format,
			align);
		if (ret < 0)
			return ret;
	}

	if (planes > AV_NUM_DATA_POINTERS) {
		frame->extended_data = (uint8_t**)av_mallocz_array(planes,
			sizeof(*frame->extended_data));
		frame->extended_buf = (AVBufferRef**)av_mallocz_array((planes - AV_NUM_DATA_POINTERS),
			sizeof(*frame->extended_buf));
		if (!frame->extended_data || !frame->extended_buf) {
			av_freep(&frame->extended_data);
			av_freep(&frame->extended_buf);
			return AVERROR(ENOMEM);
		}
		frame->nb_extended_buf = planes - AV_NUM_DATA_POINTERS;
	}
	else
		frame->extended_data = frame->data;

	printf("FFMIN(planes, AV_NUM_DATA_POINTERS):%d\n", FFMIN(planes, AV_NUM_DATA_POINTERS));
	for (i = 0; i < FFMIN(planes, AV_NUM_DATA_POINTERS); i++) {
		printf("================(frame->linesize[0]:%d\n", frame->linesize[0]);
		printf("i:%d buf[0]:%p\n", i, frame->buf[i]);
		//frame->buf[i] = av_buffer_alloc(frame->linesize[0]);
		frame->buf[i] = myav_buffer_alloc(frame->linesize[0]);
		if (!frame->buf[i]) {
			av_frame_unref(frame);
			return AVERROR(ENOMEM);
		}
		frame->extended_data[i] = frame->data[i] = frame->buf[i]->data;
	}
	for (i = 0; i < planes - AV_NUM_DATA_POINTERS; i++) {
		frame->extended_buf[i] = av_buffer_alloc(frame->linesize[0]);
		if (!frame->extended_buf[i]) {
			av_frame_unref(frame);
			return AVERROR(ENOMEM);
		}
		frame->extended_data[i + AV_NUM_DATA_POINTERS] = frame->extended_buf[i]->data;
	}
	return 0;
}

int AudioSample::createDstFrame(int channel_layout, AVSampleFormat format, int nb_samples)
{
	if (frame_) 
		return 0;
	frame_ = av_frame_alloc();
	frame_->channel_layout = channel_layout;
	frame_->format = format;
	frame_->nb_samples = nb_samples;
	if (frame_->data[0])
	{
		av_freep(&frame_->data[0]);
	}

	int ret = my_get_audio_buffer(frame_, 0);
	if (ret != 0) {
		av_log(NULL, AV_LOG_ERROR, "open input failure.[%d][%s]\n", AVERROR(ret));
		return -1;
	}
	frame_->nb_samples = nb_samples;
	printf("channel_layout:%d format:%d nb_samples:%d frame_->line_size:%d", 
		channel_layout, format, nb_samples, frame_->linesize[0]);
	return 0;
}

int AudioSample::audioSampleCreateData()
{
	// 创建重采样输出缓冲区
	av_samples_alloc_array_and_samples(
		&dstData_, //输出缓冲区地址
		&dstLen_, //缓冲区大小
		av_get_channel_layout_nb_channels(dstChLayout_), //通道个数 av_get_channel_layout_nb_channels(ch_layout)
		1024,    //采样个数 4096(字节)/2(采样位数16 bit)/2通道
		dstFormat_, //采样格式 根据这三个就能算出来需要的总字节, 和 采集的时候 frame->data 算的长度一致
		0);
	
	// 创建重采样输入缓冲区
	av_samples_alloc_array_and_samples(
		&srcData_, //输出缓冲区地址
		&srcLen_, //缓冲区大小
		av_get_channel_layout_nb_channels(srcChLayout_), //通道个数 av_get_channel_layout_nb_channels(ch_layout)
		1024,    //采样个数 4096(字节)/2(采样位数16 bit)/2通道
		srcFormat_, //采样格式 根据这三个就能算出来需要的总字节, 和 采集的时候 frame->data 算的长度一致
		0);
	printf("samples[%d] dstformaot[%d] dstChLayout_[%d] create sample dst data len = %d\n", 
		1024, dstFormat_, dstChLayout_, dstLen_);
	printf("samples[%d] srcformaot[%d] srcChLayout_[%d] create sample src data len = %d\n",
		1024, srcFormat_, srcChLayout_, srcLen_, srcLen_);
	return 0;
}

int AudioSample::audioSampleConvert(AVFrame *srcFrame, AVFrame **dstFrame)
{
	int planar = 0;
	
	planar = av_sample_fmt_is_planar(srcFormat_);
	printf(" sdfsdf srcFrame->linesize[0]:%d planar:%d\n", srcFrame->linesize[0], planar);
	if (planar)
	{
		printf("srcFrame->linesize[0]:%d\n", srcFrame->linesize[0]);
		int channels = av_get_channel_layout_nb_channels(srcChLayout_);
		//理论上plannar模式下srcFrame->linesize[0]存放的是每个通道的大小，但是次处却是所有通道的总的size
		int plane_len = srcFrame->linesize[0]/channels;    // 每层的数据长度
		for (int i = 0; i < channels; i++) {               // 将每层数据写给源缓冲区
			//解码的时候是从fltp->s16
			//按理论上是以下的方式处理，但存在问题，此处先记录下
			memcpy((void*)(srcData_[0]+ plane_len*i), srcFrame->data[i], plane_len);
		}
	}
	else
	{
		memcpy((void*)srcData_[0], srcFrame->data[0], srcFrame->linesize[0]);
	}
	int nb_samples = swr_convert(swrCtx_, dstData_, 1024, (const uint8_t **)srcData_, 1024);
	//int dst_linesize = 0;
	//int dst_bufsize = av_samples_get_buffer_size(&dst_linesize, av_get_channel_layout_nb_channels(dstChLayout_),
	//	nb_samples, dstFormat_, 0); 
	
	//printf("convert dst_bufsize = %d, dst_linesize = %d, nb_samples = %d\n", dst_bufsize, dst_linesize, nb_samples);
	
	printf("dstChLayout_:%d dstFormat_:%d nb_samples:%d\n", dstChLayout_, dstFormat_, nb_samples);
	createDstFrame(dstChLayout_, dstFormat_, nb_samples);
	planar = av_sample_fmt_is_planar(dstFormat_);
	printf("planar:[%d]----convert create dst frame[0] size = %d\n", planar, frame_->linesize[0]);
	if (planar) {
		int channels = av_get_channel_layout_nb_channels(srcChLayout_);
		int plane_len = frame_->linesize[0];               // 每层的数据长度
		for (int i = 0; i < channels; i++) {               // 将每层数据写给源缓冲区
			memcpy(frame_->data[i], dstData_[0] + plane_len*i, plane_len);
		}
	} else {
		memcpy(frame_->data[0], dstData_[0], frame_->linesize[0]);
	}
	
	*dstFrame = frame_;
	return 0;
}

AudioEncode::~AudioEncode()
{
}

int AudioEncode::audioEncodeInit(AVSampleFormat encodeFormat, int encodeChLayout, int sampleRate, int bitRate, int profile)
{
	AVCodec *codec = avcodec_find_encoder_by_name(encoderName_.c_str());
	if (!codec) {
		av_log(NULL, AV_LOG_ERROR, "audio not find encoder : %s\n", encoderName_.c_str());
		return -1;
	}
	encodecCtx_ = avcodec_alloc_context3(codec);
	if (!encodecCtx_) {
		av_log(NULL, AV_LOG_ERROR, "avcodec alloc ctx failed.\n");
		return -1;
	}

	audio_set_encodec_ctx(encodeFormat, encodeChLayout, sampleRate, bitRate, profile);

	int ret = avcodec_open2(encodecCtx_, codec, NULL);
	if (ret != 0) {
		av_log(NULL, AV_LOG_ERROR, "avcodec open 2 failed.\n");
		return -1;
	}
	av_init_packet(&packet_);
	profile_ = profile;
	channels_ = av_get_channel_layout_nb_channels(encodeChLayout);
	sampleRate_ = sampleRate;
	return 0;
}

int AudioEncode::audioEncode(AVFrame *frame, AVPacket **packet)
{
	int ret = avcodec_send_frame(encodecCtx_, frame);
	//ret >= 0说明数据设置成功了
	while (ret >= 0) {
		ret = avcodec_receive_packet(encodecCtx_, &packet_);
		//要一直获取知道失败，因为可能会有好多帧需要吐出来，
		if (ret < 0) {
			//说明没有帧了，要继续送数据
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
				break;
			}
			else {
				return -1;//其他的值，说明真失败了。
			}
		}
		break;  //获取到一个packet
	}
	*packet = &packet_;
	return ret;
}

/* |- syncword(12) ...             | ID(v)(1)|    layer(2)  | protection_absent(1)|  (16bit)
** |- profile(2)   | private_bit(1)| sample_rate_index(4) | channel_nb(1)|(8bit) //通道数buf[2]只填了1bit,剩下的2bit在buf[3]
** | -channel_nb(2)|

*/
void AudioEncode::packetAddHeader(char *aac_header, int frame_len)
{
	int sampling_frequency_index = sampleIndex.at(sampleRate_);
	printf("%d\n", sampling_frequency_index);
	printf("add header sample rate :%d, index: %d\n", sampleRate_, sampling_frequency_index);
	//sync word
	aac_header[0] = 0xff;         //syncword:0xfff                          高8bits
	aac_header[1] = 0xf0;         //syncword:0xfff                          低4bits
	//ID
	aac_header[1] |= (0 << 3);    //ID : 0 for MPEG-4 ;  1 for MPEG-2  
	//layer
	aac_header[1] |= (0 << 1);    //Layer: always:00
	//protection absent
	aac_header[1] |= 1;           //protection absent:1    set to 1 if there is no CRC and 0 if there is CRC
	// also : aac_header[0] = 0xff; aac_header[1] = 0xf1;
	//profile
	aac_header[2] = (profile_+1) << 6;  //profile:profile  2bits  和 MPEG-4 Audio Object Type = profile + 1
	//sampling_frequency_index
	aac_header[2] |= (sampling_frequency_index & 0x0f) << 2; //sampling_frequency_index 4bits 只有4bit要 &0x0f，清空高4位 
	//private_bit
	aac_header[2] |= (0 << 1);        //private_bit: 0   1bits      
	//channels
	aac_header[2] |= (channels_ & 0x04) >> 2;  //channel 高1bit: &0000 0100取channels的最高位
	aac_header[3] |= (channels_ & 0x03) << 6;  //&0000 0011取channels的低2位

	aac_header[3] |= (0 << 5);               //original：0                1bit
	aac_header[3] |= (0 << 4);               //home：0                    1bit
	aac_header[3] |= (0 << 3);               //copyright id bit：0        1bit
	aac_header[3] |= (0 << 2);               //copyright id start：0      1bit

	//frame len 一个ADTS帧的长度包括ADTS头和AAC原始流
	int adtsLen = frame_len + 7;
	aac_header[3] |= ((adtsLen & 0x1800) >> 11);           //frame length：value   高2bits 
	aac_header[4] = (uint8_t)((adtsLen & 0x7f8) >> 3);     //frame length:value    中间8bits
	aac_header[5] = (uint8_t)((adtsLen & 0x7) << 5);       //frame length:value    低3bits
	//buffer fullness 0x7FF 说明是码率可变的码流
	aac_header[5] |= 0x1f;                                 //buffer fullness:0x7ff 高5bits
	aac_header[6] = 0xfc;      //?11111100?                  //buffer fullness:0x7ff 低6bits
	return ;
}
void AudioEncode::packetAddHeader(char *aac_header, int profile, int sample_index, int channels, int frame_len)
{
	int sampling_frequency_index = sampleIndex.at(sampleRate_);
	printf("add header sample rate :%d, index: %d\n", sampleRate_, sampling_frequency_index);
	//sync word
	aac_header[0] = 0xff;         //syncword:0xfff                          高8bits
	aac_header[1] = 0xf0;         //syncword:0xfff                          低4bits
								  //ID
	aac_header[1] |= (0 << 3);    //ID : 0 for MPEG-4 ;  1 for MPEG-2  
								  //layer
	aac_header[1] |= (0 << 1);    //Layer:0  always:00
								  //protection absent
	aac_header[1] |= 1;           //protection absent:1    前两个字节的最低位为 1 
								  // also : aac_header[0] = 0xff; aac_header[1] = 0xf1;
								  //profile
	aac_header[2] = (profile_) << 6;  //profile:profile  2bits
									  //sampling_frequency_index
	aac_header[2] |= (sampling_frequency_index & 0x0f) << 2; //sampling_frequency_index 4bits 只有4bit要 &0x0f，清空高4位 
															 //private_bit
	aac_header[2] |= (0 << 1);        //private_bit: 0   1bits      
									  //channels
	aac_header[2] |= (channels_ & 0x04) >> 2;  //channel 高1bit: &0000 0100取channels的最高位
	aac_header[3] |= (channels_ & 0x03) << 6;  //&0000 0011取channels的低2位

	aac_header[3] |= (0 << 5);               //original：0                1bit
	aac_header[3] |= (0 << 4);               //home：0                    1bit
	aac_header[3] |= (0 << 3);               //copyright id bit：0        1bit
	aac_header[3] |= (0 << 2);               //copyright id start：0      1bit

											 //frame len 一个ADTS帧的长度包括ADTS头和AAC原始流
	int adtsLen = frame_len + 7;
	aac_header[3] |= ((adtsLen & 0x1800) >> 11);           //frame length：value   高2bits 
	aac_header[4] = (uint8_t)((adtsLen & 0x7f8) >> 3);     //frame length:value    中间8bits
	aac_header[5] = (uint8_t)((adtsLen & 0x7) << 5);       //frame length:value    低3bits
														   //buffer fullness 0x7FF 说明是码率可变的码流
	aac_header[5] |= 0x1f;                                 //buffer fullness:0x7ff 高5bits
	aac_header[6] = 0xfc;      //?11111100?                  //buffer fullness:0x7ff 低6bits
	return;
}
void AudioEncode::audio_set_encodec_ctx(AVSampleFormat encodeFormat, int encodeChLayout,
										 int sampleRate, int bitRate, int profile)
{
	encodecCtx_->sample_fmt = encodeFormat;
	encodecCtx_->channel_layout = encodeChLayout;
	encodecCtx_->sample_rate = sampleRate;
	encodecCtx_->bit_rate = bitRate;
	encodecCtx_->profile = profile;
	encodecCtx_->channels = av_get_channel_layout_nb_channels(encodeChLayout);
}


AudioDecode::~AudioDecode()
{
}

int AudioDecode::AudioDecodeInit(AVSampleFormat decodeFormat, uint64_t decodeChLayout, 
	int sampleRate, int bitRate, int profile)
{
	decodeFormat_ = decodeFormat;
	sampleRate_ = sampleRate;
	profile_ = profile;
	channellayout_ = decodeChLayout;

	AVCodec* codec = avcodec_find_decoder_by_name(decoderName_.c_str());
	if (!codec) {
		av_log(NULL, AV_LOG_ERROR, "audio not find encoder : %s\n", decoderName_.c_str());
		return -1;
	}
	decodecCtx_ = avcodec_alloc_context3(codec);
	if (!decodecCtx_) {
		av_log(NULL, AV_LOG_ERROR, "avcodec alloc ctx failed.\n");
		return -1;
	}

	audio_set_decodec_ctx(decodeFormat, decodeChLayout, sampleRate, bitRate, profile);

	int ret = avcodec_open2(decodecCtx_, codec, NULL);
	if (ret != 0) {
		av_log(NULL, AV_LOG_ERROR, "avcodec open 2 failed.\n");
		return -1;
	}
	av_init_packet(&packet_);
	createdecFrame(decodeChLayout, decodeFormat, 1024);

	return 0;
}

void AudioDecode::audio_set_decodec_ctx(AVSampleFormat decodeFormat, uint64_t decodeChLayout,
	int sampleRate, int bitRate, int profile)
{
	decodecCtx_->sample_fmt = decodeFormat;
	decodecCtx_->channel_layout = decodeChLayout;
	decodecCtx_->sample_rate = sampleRate;
	decodecCtx_->bit_rate = bitRate;
	decodecCtx_->profile = profile;
	decodecCtx_->channels = av_get_channel_layout_nb_channels(decodeChLayout);
}

AVFrame* AudioDecode::createFrame(uint64_t channel_layout, AVSampleFormat format, int nb_samples)
{
	AVFrame *frame = NULL;

	frame = av_frame_alloc();
	frame->channel_layout = channel_layout;
	frame->format = format;
	frame->nb_samples = nb_samples;
	int ret = av_frame_get_buffer(frame, 0);
	if (ret != 0) {
		av_log(NULL, AV_LOG_ERROR, "open input failure.[%d][%s]\n", AVERROR(ret));
		return NULL;
	}
	return frame;
}

int AudioDecode::createdecFrame(uint64_t channel_layout, AVSampleFormat format, int nb_samples)
{
	if (decframe_)
		return 0;
	decframe_ = av_frame_alloc();
	decframe_->channel_layout = channel_layout;
	decframe_->format = format;
	decframe_->nb_samples = nb_samples;
	printf("channel_layout:%d\n", channel_layout);
	printf("format:%d\n", format);
	printf("nb_samples:%d\n", nb_samples);
	int ret = av_frame_get_buffer(decframe_, 0);
	if (ret != 0) {
		av_log(NULL, AV_LOG_ERROR, "open input failure.[%d][%s]\n", AVERROR(ret));
		return -1;
	}
	return 0;
}

int AudioDecode::AudioDecodeDeinit()
{
	return 0;
}

int AudioDecode::createInstream(string filename)
{
	int ret = 0;
	if (decode_type)
	{
		ret = avformat_open_input(&fmtCtx_, filename.c_str(), NULL, NULL);
		if (ret != 0) {
			av_strerror(ret, error, 128);
			av_log(NULL, AV_LOG_ERROR, "open input failure.[ret][%s]\n", AVERROR(ret), error);
			return -1;
		}
		av_dump_format(fmtCtx_, 0, filename.c_str(), 0);
	}
	else
	{
		printf("open file %s\n", filename.c_str());
		in_fd = fopen(filename.c_str(), "rb+");
		if (!in_fd)
		{
			av_log(NULL, AV_LOG_ERROR, "open input file %s error!\n", filename.c_str());
		}
	}
	
}

int AudioDecode::audiodecode_()
{
	printf("packet_:%d\n", packet_.size);
	int ret = avcodec_send_packet(decodecCtx_, &packet_);
	//ret >= 0说明数据设置成功了
	while (ret >= 0) {
		ret = avcodec_receive_frame(decodecCtx_, decframe_);
		//要一直获取知道失败，因为可能会有好多帧需要吐出来，
		if (ret < 0) {
			//说明没有帧了，要继续送数据
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
				break;
			}
			else {
				return -1;//其他的值，说明真失败了。
			}
		}
		break;  //获取到一个packet
	}
	return ret;
}

int get_aac_frame_len(UINT8* aac_header)
{
	int size = 0;

	if (NULL == aac_header)
	{
		return -1;
	}

	size |= (aac_header[3] & 0b00000011) << 11; //0x03  前两个最高位，要移到高位（13 - 11 = 2） 
	size |= aac_header[4] << 3;                //中间的8bit,要移到前两个高位后，13 - 2 = 11 - 8 = 3
	size |= (aac_header[5] & 0b11100000) >> 5; //0xe0 最后的3Bit，要移到最后 
	printf("size:%d\n", size);
	return size;
}

int AudioDecode::audiodecode(AVFrame **dst_frame)
{
	int ret = 0;
	
	if (decode_type)
	{
		ret = av_read_frame(fmtCtx_, &packet_);
		if (ret < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "av_read_frame error over read file over!\n");
			return ret;
		}
	}
	else
	{
		UINT8 aac_data[2048] = { 0 };
		int aac_frame_len = 0;
		ret = fread(aac_data, 1, 7, in_fd); //读aac header 7个字节
		if (ret <= 0)
		{
			perror("read:");
			av_log(NULL, AV_LOG_ERROR, "read over ret : %d!\n", ret);
			return ret;
		}
		aac_frame_len = get_aac_frame_len(aac_data);
		printf("aac_frame_len:%d\n", aac_frame_len);
		ret = fread(aac_data+7, 1, aac_frame_len - 7, in_fd);
		if (ret <= 0)
		{
			av_log(NULL, AV_LOG_ERROR, "read over !\n");
			return ret;
		}
		packet_.size = aac_frame_len;
		if (packet_.data)
			packet_.data = (uint8_t*)av_malloc(2048);
		//memcpy(packet_.data, aac_data, aac_frame_len);
		int ret = av_packet_from_data(&packet_, aac_data, packet_.size);
		if (ret != 0)
		{
			av_log(NULL, AV_LOG_ERROR, "av_packet_from_data error!\n");
			return ret;
		}
	}
	printf("[%x][%x][%x][%x][%x][%x][%x]\n",
		packet_.data[0], packet_.data[1], packet_.data[2], packet_.data[3], packet_.data[4], packet_.data[5], packet_.data[6]);
	audiodecode_();
	*dst_frame = decframe_;
	//if (packet_.data)
	//{
	//	av_free(packet_.data);
	//}
	return ret;
}
