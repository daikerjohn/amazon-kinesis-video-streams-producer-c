#include "Samples.h"

#define DEFAULT_RETENTION_PERIOD        2 * HUNDREDS_OF_NANOS_IN_AN_HOUR
#define DEFAULT_BUFFER_DURATION         120 * HUNDREDS_OF_NANOS_IN_A_SECOND
#define DEFAULT_CALLBACK_CHAIN_COUNT    5
#define DEFAULT_KEY_FRAME_INTERVAL      45
#define DEFAULT_FPS_VALUE               25
#define DEFAULT_STREAM_DURATION         20 * HUNDREDS_OF_NANOS_IN_A_SECOND
#define SAMPLE_AUDIO_FRAME_DURATION     (20 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND)
#define SAMPLE_VIDEO_FRAME_DURATION     (HUNDREDS_OF_NANOS_IN_A_SECOND / DEFAULT_FPS_VALUE)
#define AAC_AUDIO_TRACK_SAMPLING_RATE   48000
#define ALAW_AUDIO_TRACK_SAMPLING_RATE  8000
#define AAC_AUDIO_TRACK_CHANNEL_CONFIG  2
#define ALAW_AUDIO_TRACK_CHANNEL_CONFIG 1
#define AUDIO_CODEC_NAME_MAX_LENGTH     5
#define VIDEO_CODEC_NAME_MAX_LENGTH     5
#define AUDIO_CODEC_NAME_ALAW           "alaw"
#define AUDIO_CODEC_NAME_AAC            "aac"
#define VIDEO_CODEC_NAME_H264           "h264"
#define VIDEO_CODEC_NAME_H265           "h265"

#define NUMBER_OF_VIDEO_FRAME_FILES 403
#define NUMBER_OF_AUDIO_FRAME_FILES 582

// #define IOT_CORE_ENABLE_CREDENTIALS 1

UINT8 gEventsEnabled = 0;

typedef struct {
    PBYTE buffer;
    UINT32 size;
} FrameData, *PFrameData;

typedef struct {
    volatile ATOMIC_BOOL firstVideoFramePut;
    UINT64 streamStopTime;
    UINT64 streamStartTime;
    STREAM_HANDLE streamHandle;
    CHAR sampleDir[MAX_PATH_LEN + 1];
    FrameData audioFrames[NUMBER_OF_AUDIO_FRAME_FILES];
    FrameData videoFrames[NUMBER_OF_VIDEO_FRAME_FILES];
    BOOL firstFrame;
    UINT64 startTime;
} SampleCustomData, *PSampleCustomData;

PVOID putVideoFrameRoutine(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleCustomData data = (PSampleCustomData) args;
    Frame frame;
    UINT32 fileIndex = 0;
    STATUS status;
    UINT64 runningTime;
    DOUBLE startUpLatency;

    CHK(data != NULL, STATUS_NULL_ARG);

    frame.frameData = data->videoFrames[fileIndex].buffer;
    frame.size = data->videoFrames[fileIndex].size;
    frame.version = FRAME_CURRENT_VERSION;
    frame.trackId = DEFAULT_VIDEO_TRACK_ID;
    frame.duration = 0;
    frame.decodingTs = 0;
    frame.presentationTs = 0;
    frame.index = 0;

    // video track is used to mark new fragment. A new fragment is generated for every frame with FRAME_FLAG_KEY_FRAME
    frame.flags = fileIndex % DEFAULT_KEY_FRAME_INTERVAL == 0 ? FRAME_FLAG_KEY_FRAME : FRAME_FLAG_NONE;

    while (GETTIME() < data->streamStopTime) {
        status = putKinesisVideoFrame(data->streamHandle, &frame);
        if (data->firstFrame) {
            startUpLatency = (DOUBLE) (GETTIME() - data->startTime) / (DOUBLE) HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
            DLOGD("Start up latency: %lf ms", startUpLatency);
            data->firstFrame = FALSE;
        } else if (frame.flags == FRAME_FLAG_KEY_FRAME && gEventsEnabled) {
            // generate an image and notification event at the start of the video stream.
            putKinesisVideoEventMetadata(data->streamHandle, STREAM_EVENT_TYPE_NOTIFICATION | STREAM_EVENT_TYPE_IMAGE_GENERATION, NULL);
            // only push this once in this sample. A customer may use this whenever it is necessary though
            gEventsEnabled = 0;
        }

        ATOMIC_STORE_BOOL(&data->firstVideoFramePut, TRUE);
        if (STATUS_FAILED(status)) {
            printf("putKinesisVideoFrame failed with 0x%08x\n", status);
            status = STATUS_SUCCESS;
        }

        frame.presentationTs += SAMPLE_VIDEO_FRAME_DURATION;
        frame.decodingTs = frame.presentationTs;
        frame.index++;

        fileIndex = (fileIndex + 1) % NUMBER_OF_VIDEO_FRAME_FILES;
        frame.flags = fileIndex % DEFAULT_KEY_FRAME_INTERVAL == 0 ? FRAME_FLAG_KEY_FRAME : FRAME_FLAG_NONE;
        frame.frameData = data->videoFrames[fileIndex].buffer;
        frame.size = data->videoFrames[fileIndex].size;

        // synchronize putKinesisVideoFrame to running time
        runningTime = GETTIME() - data->streamStartTime;
        if (runningTime < frame.presentationTs) {
            // reduce sleep time a little for smoother video
            THREAD_SLEEP((frame.presentationTs - runningTime) * 0.9);
        }
    }

CleanUp:

    if (retStatus != STATUS_SUCCESS) {
        printf("putVideoFrameRoutine failed with 0x%08x", retStatus);
    }

    return (PVOID) (ULONG_PTR) retStatus;
}

PVOID putAudioFrameRoutine(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleCustomData data = (PSampleCustomData) args;
    Frame frame;
    UINT32 fileIndex = 0;
    STATUS status;
    UINT64 runningTime;

    CHK(data != NULL, STATUS_NULL_ARG);

    frame.frameData = data->audioFrames[fileIndex].buffer;
    frame.size = data->audioFrames[fileIndex].size;
    frame.version = FRAME_CURRENT_VERSION;
    frame.trackId = DEFAULT_AUDIO_TRACK_ID;
    frame.duration = 0;
    frame.decodingTs = 0;     // relative time mode
    frame.presentationTs = 0; // relative time mode
    frame.index = 0;
    frame.flags = FRAME_FLAG_NONE; // audio track is not used to cut fragment

    while (GETTIME() < data->streamStopTime) {
        // no audio can be put until first video frame is put
        if (ATOMIC_LOAD_BOOL(&data->firstVideoFramePut)) {
            status = putKinesisVideoFrame(data->streamHandle, &frame);
            if (STATUS_FAILED(status)) {
                printf("putKinesisVideoFrame for audio failed with 0x%08x\n", status);
                status = STATUS_SUCCESS;
            }

            frame.presentationTs += SAMPLE_AUDIO_FRAME_DURATION;
            frame.decodingTs = frame.presentationTs;
            frame.index++;

            fileIndex = (fileIndex + 1) % NUMBER_OF_AUDIO_FRAME_FILES;
            frame.frameData = data->audioFrames[fileIndex].buffer;
            frame.size = data->audioFrames[fileIndex].size;

            // synchronize putKinesisVideoFrame to running time
            runningTime = GETTIME() - data->streamStartTime;
            if (runningTime < frame.presentationTs) {
                THREAD_SLEEP(frame.presentationTs - runningTime);
            }
        }
    }

CleanUp:

    if (retStatus != STATUS_SUCCESS) {
        printf("putAudioFrameRoutine failed with 0x%08x", retStatus);
    }

    return (PVOID) (ULONG_PTR) retStatus;
}

INT32 main(INT32 argc, CHAR* argv[])
{
    PDeviceInfo pDeviceInfo = NULL;
    PStreamInfo pStreamInfo = NULL;
    PClientCallbacks pClientCallbacks = NULL;
    PStreamCallbacks pStreamCallbacks = NULL;
    CLIENT_HANDLE clientHandle = INVALID_CLIENT_HANDLE_VALUE;
    STREAM_HANDLE streamHandle = INVALID_STREAM_HANDLE_VALUE;
    STATUS retStatus = STATUS_SUCCESS;
    PCHAR accessKey = NULL, secretKey = NULL, sessionToken = NULL, streamName = NULL, region = NULL, cacertPath = NULL;
    UINT64 streamStopTime, streamingDuration = DEFAULT_STREAM_DURATION, fileSize = 0;
    TID audioSendTid, videoSendTid;
    SampleCustomData data;
    UINT32 i;
    CHAR filePath[MAX_PATH_LEN + 1];
    PTrackInfo pAudioTrack = NULL;
    CHAR audioCodec[AUDIO_CODEC_NAME_MAX_LENGTH] = {0};
    CHAR videoCodec[VIDEO_CODEC_NAME_MAX_LENGTH] = {0};
    BYTE aacAudioCpd[KVS_AAC_CPD_SIZE_BYTE];
    BYTE alawAudioCpd[KVS_PCM_CPD_SIZE_BYTE];
    VIDEO_CODEC_ID videoCodecID = VIDEO_CODEC_ID_H264;
    CHAR endpointOverride[MAX_URI_CHAR_LEN];

    MEMSET(&data, 0x00, SIZEOF(SampleCustomData));

    SNPRINTF(audioCodec, SIZEOF(audioCodec), "%s", AUDIO_CODEC_NAME_AAC);  // aac audio by default
    SNPRINTF(videoCodec, SIZEOF(videoCodec), "%s", VIDEO_CODEC_NAME_H264); // h264 video by default

#ifdef IOT_CORE_ENABLE_CREDENTIALS
    PCHAR pIotCoreCredentialEndpoint, pIotCoreCert, pIotCorePrivateKey, pIotCoreRoleAlias, pIotCoreThingName;
    CHK_ERR((pIotCoreCredentialEndpoint = GETENV(IOT_CORE_CREDENTIAL_ENDPOINT)) != NULL, STATUS_INVALID_OPERATION,
            "AWS_IOT_CORE_CREDENTIAL_ENDPOINT must be set");
    CHK_ERR((pIotCoreCert = GETENV(IOT_CORE_CERT)) != NULL, STATUS_INVALID_OPERATION, "AWS_IOT_CORE_CERT must be set");
    CHK_ERR((pIotCorePrivateKey = GETENV(IOT_CORE_PRIVATE_KEY)) != NULL, STATUS_INVALID_OPERATION, "AWS_IOT_CORE_PRIVATE_KEY must be set");
    CHK_ERR((pIotCoreRoleAlias = GETENV(IOT_CORE_ROLE_ALIAS)) != NULL, STATUS_INVALID_OPERATION, "AWS_IOT_CORE_ROLE_ALIAS must be set");
    CHK_ERR((pIotCoreThingName = GETENV(IOT_CORE_THING_NAME)) != NULL, STATUS_INVALID_OPERATION, "AWS_IOT_CORE_THING_NAME must be set");
#else
    if (argc < 2) {
        printf("Usage: AWS_ACCESS_KEY_ID=SAMPLEKEY AWS_SECRET_ACCESS_KEY=SAMPLESECRET %s <stream_name> <duration_in_seconds> <frame_files_path> "
               "[audio_codec] [video_codec] [events_enabled]\n",
               argv[0]);
        CHK(FALSE, STATUS_INVALID_ARG);
    }
    if ((accessKey = GETENV(ACCESS_KEY_ENV_VAR)) == NULL || (secretKey = GETENV(SECRET_KEY_ENV_VAR)) == NULL) {
        printf("Error missing credentials\n");
        CHK(FALSE, STATUS_INVALID_ARG);
    }
    sessionToken = GETENV(SESSION_TOKEN_ENV_VAR);
#endif

    if (argc == 7) {
        if (!STRCMP(argv[6], "1")) {
            gEventsEnabled = 1;
        }
    }
    if (argc >= 6) {
        if (!STRCMP(argv[4], AUDIO_CODEC_NAME_ALAW)) {
            SNPRINTF(audioCodec, SIZEOF(audioCodec), "%s", AUDIO_CODEC_NAME_ALAW);
        }
        if (!STRCMP(argv[5], VIDEO_CODEC_NAME_H265)) {
            STRCPY(videoCodec, VIDEO_CODEC_NAME_H265);
            videoCodecID = VIDEO_CODEC_ID_H265;
        }
    }

    MEMSET(data.sampleDir, 0x00, MAX_PATH_LEN + 1);
    if (argc < 4) {
        STRCPY(data.sampleDir, (PCHAR) "../samples");
    } else {
        STRNCPY(data.sampleDir, argv[3], MAX_PATH_LEN);
        if (data.sampleDir[STRLEN(data.sampleDir) - 1] == '/') {
            data.sampleDir[STRLEN(data.sampleDir) - 1] = '\0';
        }
    }

    printf("Loading audio frames...\n");
    for (i = 0; i < NUMBER_OF_AUDIO_FRAME_FILES; ++i) {
        SNPRINTF(filePath, MAX_PATH_LEN, "%s/%sSampleFrames/sample-%03d.%s", data.sampleDir, audioCodec, i + 1, audioCodec);
        CHK_STATUS(readFile(filePath, TRUE, NULL, &fileSize));
        data.audioFrames[i].buffer = (PBYTE) MEMALLOC(fileSize);
        data.audioFrames[i].size = fileSize;
        CHK_STATUS(readFile(filePath, TRUE, data.audioFrames[i].buffer, &fileSize));
    }
    printf("Done loading audio frames.\n");

    printf("Loading video frames...\n");
    for (i = 0; i < NUMBER_OF_VIDEO_FRAME_FILES; ++i) {
        SNPRINTF(filePath, MAX_PATH_LEN, "%s/%sSampleFrames/frame-%03d.%s", data.sampleDir, videoCodec, i + 1, videoCodec);
        CHK_STATUS(readFile(filePath, TRUE, NULL, &fileSize));
        data.videoFrames[i].buffer = (PBYTE) MEMALLOC(fileSize);
        data.videoFrames[i].size = fileSize;
        CHK_STATUS(readFile(filePath, TRUE, data.videoFrames[i].buffer, &fileSize));
    }
    printf("Done loading video frames.\n");

    cacertPath = GETENV(CACERT_PATH_ENV_VAR);

#ifdef IOT_CORE_ENABLE_CREDENTIALS
    streamName = pIotCoreThingName;
#else
    streamName = argv[1];
#endif

    if ((region = GETENV(DEFAULT_REGION_ENV_VAR)) == NULL) {
        region = (PCHAR) DEFAULT_AWS_REGION;
    }

    if (argc >= 3) {
        // Get the duration and convert to an integer
        CHK_STATUS(STRTOUI64(argv[2], NULL, 10, &streamingDuration));
        printf("streaming for %" PRIu64 " seconds\n", streamingDuration);
        streamingDuration *= HUNDREDS_OF_NANOS_IN_A_SECOND;
    }

    streamStopTime = GETTIME() + streamingDuration;

    // default storage size is 128MB. Use setDeviceInfoStorageSize after create to change storage size.
    CHK_STATUS(createDefaultDeviceInfo(&pDeviceInfo));
    // adjust members of pDeviceInfo here if needed
    pDeviceInfo->clientInfo.loggerLogLevel = LOG_LEVEL_DEBUG;

    // generate audio cpd
    if (!STRCMP(audioCodec, AUDIO_CODEC_NAME_ALAW)) {
        CHK_STATUS(createRealtimeAudioVideoStreamInfoProviderWithCodecs(streamName, DEFAULT_RETENTION_PERIOD, DEFAULT_BUFFER_DURATION, videoCodecID,
                                                                        AUDIO_CODEC_ID_PCM_ALAW, &pStreamInfo));

        // adjust members of pStreamInfo here if needed

        // set up audio cpd.
        pAudioTrack = pStreamInfo->streamCaps.trackInfoList[0].trackId == DEFAULT_AUDIO_TRACK_ID ? &pStreamInfo->streamCaps.trackInfoList[0]
                                                                                                 : &pStreamInfo->streamCaps.trackInfoList[1];
        pAudioTrack->codecPrivateData = alawAudioCpd;
        pAudioTrack->codecPrivateDataSize = KVS_PCM_CPD_SIZE_BYTE;
        CHK_STATUS(mkvgenGeneratePcmCpd(KVS_PCM_FORMAT_CODE_ALAW, ALAW_AUDIO_TRACK_SAMPLING_RATE, ALAW_AUDIO_TRACK_CHANNEL_CONFIG,
                                        pAudioTrack->codecPrivateData, pAudioTrack->codecPrivateDataSize));
    } else {
        CHK_STATUS(createRealtimeAudioVideoStreamInfoProviderWithCodecs(streamName, DEFAULT_RETENTION_PERIOD, DEFAULT_BUFFER_DURATION, videoCodecID,
                                                                        AUDIO_CODEC_ID_AAC, &pStreamInfo));

        // CHK_STATUS(createRealtimeAudioVideoStreamInfoProvider(streamName, DEFAULT_RETENTION_PERIOD, DEFAULT_BUFFER_DURATION, &pStreamInfo));
        // You can use createRealtimeAudioVideoStreamInfoProvider for H.264 and AAC as it uses them by default
        // To specify PCM/G.711 use createRealtimeAudioVideoStreamInfoProviderWithCodecs
        // adjust members of pStreamInfo here if needed

        // set up audio cpd.
        pAudioTrack = pStreamInfo->streamCaps.trackInfoList[0].trackId == DEFAULT_AUDIO_TRACK_ID ? &pStreamInfo->streamCaps.trackInfoList[0]
                                                                                                 : &pStreamInfo->streamCaps.trackInfoList[1];
        pAudioTrack->codecPrivateData = aacAudioCpd;
        pAudioTrack->codecPrivateDataSize = KVS_AAC_CPD_SIZE_BYTE;
        CHK_STATUS(mkvgenGenerateAacCpd(AAC_LC, AAC_AUDIO_TRACK_SAMPLING_RATE, AAC_AUDIO_TRACK_CHANNEL_CONFIG, pAudioTrack->codecPrivateData,
                                        pAudioTrack->codecPrivateDataSize));
    }

    // use relative time mode. Buffer timestamps start from 0
    pStreamInfo->streamCaps.absoluteFragmentTimes = FALSE;

    data.startTime = GETTIME();
    data.firstFrame = TRUE;

    getEndpointOverride(endpointOverride, SIZEOF(endpointOverride));
#ifdef IOT_CORE_ENABLE_CREDENTIALS
    CHK_STATUS(createDefaultCallbacksProviderWithIotCertificateAndEndpointOverride(pIotCoreCredentialEndpoint, pIotCoreCert, pIotCorePrivateKey,
                                                                                   cacertPath, pIotCoreRoleAlias, pIotCoreThingName, region, NULL,
                                                                                   NULL, endpointOverride, &pClientCallbacks));
#else
    CHK_STATUS(createDefaultCallbacksProviderWithAwsCredentialsAndEndpointOverride(accessKey, secretKey, sessionToken, MAX_UINT64, region, cacertPath,
                                                                                   NULL, NULL, endpointOverride, &pClientCallbacks));
#endif

    if (NULL != GETENV(ENABLE_FILE_LOGGING)) {
        if ((retStatus = addFileLoggerPlatformCallbacksProvider(pClientCallbacks, FILE_LOGGING_BUFFER_SIZE, MAX_NUMBER_OF_LOG_FILES,
                                                                (PCHAR) FILE_LOGGER_LOG_FILE_DIRECTORY_PATH, TRUE) != STATUS_SUCCESS)) {
            printf("File logging enable option failed with 0x%08x error code\n", retStatus);
        }
    }

    CHK_STATUS(createStreamCallbacks(&pStreamCallbacks));
    CHK_STATUS(addStreamCallbacks(pClientCallbacks, pStreamCallbacks));

    CHK_STATUS(createKinesisVideoClient(pDeviceInfo, pClientCallbacks, &clientHandle));
    CHK_STATUS(createKinesisVideoStreamSync(clientHandle, pStreamInfo, &streamHandle));

    data.streamStopTime = streamStopTime;
    data.streamHandle = streamHandle;
    data.streamStartTime = GETTIME();
    ATOMIC_STORE_BOOL(&data.firstVideoFramePut, FALSE);

    THREAD_CREATE(&videoSendTid, putVideoFrameRoutine, (PVOID) &data);
    THREAD_CREATE(&audioSendTid, putAudioFrameRoutine, (PVOID) &data);

    THREAD_JOIN(videoSendTid, NULL);
    THREAD_JOIN(audioSendTid, NULL);

    CHK_STATUS(stopKinesisVideoStreamSync(streamHandle));
    CHK_STATUS(freeKinesisVideoStream(&streamHandle));
    CHK_STATUS(freeKinesisVideoClient(&clientHandle));

CleanUp:

    if (STATUS_FAILED(retStatus)) {
        printf("Failed with status 0x%08x\n", retStatus);
    }

    for (i = 0; i < NUMBER_OF_AUDIO_FRAME_FILES; ++i) {
        SAFE_MEMFREE(data.audioFrames[i].buffer);
    }

    for (i = 0; i < NUMBER_OF_VIDEO_FRAME_FILES; ++i) {
        SAFE_MEMFREE(data.videoFrames[i].buffer);
    }
    freeDeviceInfo(&pDeviceInfo);
    freeStreamInfoProvider(&pStreamInfo);
    freeKinesisVideoStream(&streamHandle);
    freeKinesisVideoClient(&clientHandle);
    freeCallbacksProvider(&pClientCallbacks);

    return (INT32) retStatus;
}
