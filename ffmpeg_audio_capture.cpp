
#include <iostream>
#ifdef _MSC_VER
#include <windows.h>
#include <tchar.h>
#include <dshow.h>
#include <atlcomcli.h>
#pragma comment(lib, "Strmiids.lib")
#endif
#include "audio_engine.h"

void getAudioDevices(char* name)
{
#ifdef _MSC_VER
	CoInitialize(NULL);
	CComPtr<ICreateDevEnum> pCreateDevEnum;
	HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum, (void**)&pCreateDevEnum);
	CComPtr<IEnumMoniker> pEm;
	hr = pCreateDevEnum->CreateClassEnumerator(CLSID_AudioInputDeviceCategory, &pEm, 0);
	if (hr != NOERROR) {
		return;
	}
	pEm->Reset();
	ULONG cFetched;
	IMoniker* pM = NULL;
	while (hr = pEm->Next(1, &pM, &cFetched), hr == S_OK)
	{
		IPropertyBag* pBag = 0;
		hr = pM->BindToStorage(0, 0, IID_IPropertyBag, (void**)&pBag);
		if (SUCCEEDED(hr))
		{
			VARIANT var;
			var.vt = VT_BSTR;
			hr = pBag->Read(L"FriendlyName", &var, NULL); //还有其他属性,像描述信息等等...
			if (hr == NOERROR)
			{
				//获取设备名称
				WideCharToMultiByte(CP_ACP, 0, var.bstrVal, -1, name, 128, "", NULL);
				SysFreeString(var.bstrVal);
			}
			pBag->Release();
		}
		pM->Release();
	}

	pCreateDevEnum = NULL;
	pEm = NULL;
#else
	memcpy(name, "default", strlen("default") + 1);
#endif

}
#ifdef _MSC_VER
void convert(const char* strIn, char* strOut, int sourceCodepage, int targetCodepage)
{
	//LPCTSTR
	LPCTSTR pStr = (LPCTSTR)strIn;
	int len = lstrlen(pStr);
	int unicodeLen = MultiByteToWideChar(sourceCodepage, 0, strIn, -1, NULL, 0);
	wchar_t* pUnicode = NULL;
	pUnicode = new wchar_t[unicodeLen + 1];
	memset(pUnicode, 0, (unicodeLen + 1) * sizeof(wchar_t));
	MultiByteToWideChar(sourceCodepage, 0, strIn, -1, (LPWSTR)pUnicode, unicodeLen);

	BYTE* pTargetData = NULL;
	int targetLen = WideCharToMultiByte(targetCodepage, 0, (LPWSTR)pUnicode, -1, (char*)pTargetData, 0, NULL, NULL);

	pTargetData = new BYTE[targetLen + 1];
	memset(pTargetData, 0, targetLen + 1);
	WideCharToMultiByte(targetCodepage, 0, (LPWSTR)pUnicode, -1, (char*)pTargetData, targetLen, NULL, NULL);
	lstrcpy((LPSTR)strOut, (LPCSTR)pTargetData);

	delete pUnicode;
	delete pTargetData;
}
#endif

//采集编码
#if 0
int main() {
	char device_name[128] = { 0 };
	char adts_buffer[7] = { 0 };
	AVFrame* frame = NULL;
	AVFrame* resample_frame = NULL;
	AVPacket* packet = NULL;
	const char* file_name = "capture.pcm";
	FILE* fd = fopen(file_name, "wb+");

	const char* file_name1 = "sample.pcm";
	FILE* fd1 = fopen(file_name1, "wb+");

	const char* file_name2 = "encode.aac";
	FILE* fd2 = fopen(file_name2, "wb+");
#ifdef _MSC_VER	
	char name[128] = { 0 };
	char name_utf8[128] = { 0 };
	getAudioDevices(name);
	convert(name, name_utf8, CP_ACP, CP_UTF8);
	sprintf(device_name, "audio=%s", name_utf8);
	printf("device_name:%s\n", device_name);
	string libName("dshow");
#elif __APPLE__
	device_name = ":0";
	string libName("avfoundation");
#endif

	string devName(device_name);
	AudioCapture* audioCapture = new AudioCapture(devName, libName);
	printf("AV_SAMPLE_FMT_S16:%d\n", AV_SAMPLE_FMT_S16);
	int ret = audioCapture->audioInit(AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, 1024);
	if (ret < 0) {
		printf("init fail.\n");
		return 0;
	}
	AudioSample* audioSample = new AudioSample(44100, AV_SAMPLE_FMT_S16, AV_CH_LAYOUT_STEREO,
		44100, AV_SAMPLE_FMT_FLTP, AV_CH_LAYOUT_STEREO);
	audioSample->audioSampleInit();

	string encoderName("aac");
	AudioEncode* audioEncode = new AudioEncode(encoderName);
	ret = audioEncode->audioEncodeInit(AV_SAMPLE_FMT_FLTP, AV_CH_LAYOUT_STEREO, 44100, 16000, FF_PROFILE_AAC_HE);
	if (ret != 0) {
		printf("audio encode init fail.\n");
		return 0;
	}
	while (1) {
		ret = audioCapture->audioCaptureFrame(&frame);
		if (ret < 0) {
			if (ret == -35)
				continue;
			exit(0);
		}
		fwrite(frame->data[0], 1, frame->linesize[0], fd);

		printf("ssss frame linesize size = %d\n", frame->linesize[0]);
		audioSample->audioSampleConvert(frame, &resample_frame);
		//重采样的数据是 planar 模式 AV_SAMPLE_FMT_FLTP
		fwrite(resample_frame->data[0], 1, resample_frame->linesize[0], fd1);
		fwrite(resample_frame->data[1], 1, resample_frame->linesize[0], fd1);
		printf("sample frame linesize size = %d\n", resample_frame->linesize[0]);
		fflush(fd);
		fflush(fd1);

		ret = audioEncode->audioEncode(resample_frame, &packet);
		if (ret == AVERROR(EAGAIN))
		{
			continue;
		}
		if (ret == -1)
			break;
		printf("encode packet size = %d\n", packet->size);
		audioEncode->packetAddHeader(adts_buffer, packet->size);
		fwrite(adts_buffer, 1, 7, fd2);
		fwrite(packet->data, 1, packet->size, fd2);
		fflush(fd2);
	}
	audioEncode->audioEncode(NULL, &packet); //结束之后要送一个空数据，让编码器吐出缓存的数据。
}
#else

//解码aac
int main(int argc, char* argv[])
{
	int ret = 0;
	const char* out_file_name = "out_s16.pcm";
	FILE* out_fd;
	const string in_file_name = "encode.aac";
	AudioDecode* audio_decode = new AudioDecode("aac", 0);
	AVFrame* decframe = NULL;
	AVFrame* resample_frame = NULL;

	AudioSample* audioSample = new AudioSample(44100, AV_SAMPLE_FMT_FLTP, AV_CH_LAYOUT_STEREO,
		44100, AV_SAMPLE_FMT_S16, AV_CH_LAYOUT_STEREO);
	
	audioSample->audioSampleInit();

	audio_decode->createInstream(in_file_name);

	ret = audio_decode->AudioDecodeInit(AV_SAMPLE_FMT_FLTP, AV_CH_LAYOUT_STEREO, 44100, 16000, FF_PROFILE_AAC_HE);
	if (ret < 0)
	{
		cout << "Error, AudioDecodeInit aac !" << endl;
	}

	out_fd = fopen(out_file_name, "wb+");
	if (!out_fd)
	{
		cout << "open file :" << out_file_name << " error!" << endl;
		goto __FAIL;
	}

	while (1)
	{
		//解码 aac->pcm-fltp
		ret = audio_decode->audiodecode(&decframe);
		if (ret <= 0)
		{
			break;
		}
		//重采样 fltp->s16
		audioSample->audioSampleConvert(decframe, &resample_frame);

		int planar = av_sample_fmt_is_planar(AV_SAMPLE_FMT_S16);
		if (planar) {
			fwrite(resample_frame->data[0], 1, resample_frame->linesize[0], out_fd);
			fwrite(resample_frame->data[1], 1, resample_frame->linesize[0], out_fd);
		}
		else {
			fwrite(resample_frame->data[0], 1, resample_frame->linesize[0], out_fd);
		}	
	}

__FAIL:

	return 0;
}
#endif