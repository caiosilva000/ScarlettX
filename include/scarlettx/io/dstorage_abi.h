#pragma once

#include <cstdint>

#if defined(_WIN32)
#  if defined(SCARLETTX_BUILDING_DSTORAGE)
#    define SCARLETTX_DSTORAGE_API __declspec(dllexport)
#  else
#    define SCARLETTX_DSTORAGE_API __declspec(dllimport)
#  endif
#else
#  define SCARLETTX_DSTORAGE_API __attribute__((visibility("default")))
#endif

extern "C" {

enum DSTORAGE_REQUEST_SOURCE_TYPE : uint32_t {
    DSTORAGE_REQUEST_SOURCE_FILE = 0,
    DSTORAGE_REQUEST_SOURCE_MEMORY = 1,
};

enum DSTORAGE_REQUEST_DESTINATION_TYPE : uint32_t {
    DSTORAGE_REQUEST_DESTINATION_MEMORY = 0,
    DSTORAGE_REQUEST_DESTINATION_BUFFER = 1,
    DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION = 2,
};

enum DSTORAGE_PRIORITY : int8_t {
    DSTORAGE_PRIORITY_LOW = -1,
    DSTORAGE_PRIORITY_NORMAL = 0,
    DSTORAGE_PRIORITY_HIGH = 1,
    DSTORAGE_PRIORITY_REALTIME = 2,
};

enum DSTORAGE_COMPRESSION_FORMAT : uint8_t {
    DSTORAGE_COMPRESSION_FORMAT_NONE = 0,
    DSTORAGE_COMPRESSION_FORMAT_GDEFLATE = 1,
    DSTORAGE_COMPRESSION_FORMAT_LZ_RLE = 0x80,
};

struct DSTORAGE_CONFIGURATION {
    uint32_t NumSubmitThreads;
    int32_t NumBuiltInCpuDecompressionThreads;
    uint32_t ForceMappingLayer;
    uint32_t DisableBypassIO;
    uint32_t DisableTelemetry;
    uint32_t DisableGpuDecompressionMetacommand;
    uint32_t DisableGpuDecompression;
};

struct DSTORAGE_QUEUE_DESC {
    uint32_t SourceType;
    uint16_t Capacity;
    int8_t Priority;
    uint8_t Reserved;
    const char* Name;
};

struct DSTORAGE_REQUEST {
    uint32_t SourceType;
    uint32_t DestinationType;
    uint8_t CompressionFormat;
    uint8_t Reserved[3];
    // Source
    uint64_t SourceFileHandle;
    uint64_t SourceFileOffset;
    const void* SourceMemory;
    uint32_t SourceSize;
    // Destination
    void* DestinationMemory;
    uint32_t DestinationSize;
    uint32_t UncompressedSize;
    // Metadata
    uint64_t CancellationTag;
    const char* Name;
};

struct DSTORAGE_ERROR_RECORD {
    uint32_t FailureCount;
    int32_t FirstFailureHResult;
    uint32_t CommandType;
    uint32_t Reserved;
    char FailedRequestName[64];
};

typedef void (*DSTORAGE_SIGNAL_CALLBACK)(uint64_t value, void* context);

SCARLETTX_DSTORAGE_API int32_t DStorageSetConfiguration(const DSTORAGE_CONFIGURATION* config);
SCARLETTX_DSTORAGE_API int32_t DStorageGetFactory(uint64_t* out_factory);
SCARLETTX_DSTORAGE_API int32_t DStorageDestroyFactory(uint64_t factory);

SCARLETTX_DSTORAGE_API int32_t DStorageOpenFile(
    uint64_t factory,
    const char* path,
    uint64_t* out_file);
SCARLETTX_DSTORAGE_API int32_t DStorageOpenXvdFile(
    uint64_t factory,
    const char* xvd_path,
    uint64_t* out_file);
SCARLETTX_DSTORAGE_API int32_t DStorageCloseFile(uint64_t file);
SCARLETTX_DSTORAGE_API int32_t DStorageGetFileSize(uint64_t file, uint64_t* out_size);

SCARLETTX_DSTORAGE_API int32_t DStorageCreateQueue(
    uint64_t factory,
    const DSTORAGE_QUEUE_DESC* desc,
    uint64_t* out_queue);
SCARLETTX_DSTORAGE_API int32_t DStorageDestroyQueue(uint64_t queue);

SCARLETTX_DSTORAGE_API int32_t DStorageEnqueueRequest(
    uint64_t queue,
    const DSTORAGE_REQUEST* request);
// How many requests titles have enqueued in this process since it started (for activity logs).
SCARLETTX_DSTORAGE_API uint64_t DStorageXsActivityRequestCount(void);
SCARLETTX_DSTORAGE_API int32_t DStorageEnqueueSignal(
    uint64_t queue,
    uint64_t* fence_value_ptr,
    uint64_t value,
    DSTORAGE_SIGNAL_CALLBACK callback,
    void* context);
SCARLETTX_DSTORAGE_API int32_t DStorageEnqueueStatus(
    uint64_t queue,
    uint64_t status_array,
    uint32_t index);
SCARLETTX_DSTORAGE_API int32_t DStorageSubmit(uint64_t queue);
SCARLETTX_DSTORAGE_API int32_t DStorageWaitForIdle(uint64_t queue, uint32_t timeout_ms);
SCARLETTX_DSTORAGE_API int32_t DStorageCancelRequestsWithTag(
    uint64_t queue,
    uint64_t mask,
    uint64_t value);

SCARLETTX_DSTORAGE_API int32_t DStorageCreateStatusArray(
    uint64_t factory,
    uint32_t capacity,
    const char* name,
    uint64_t* out_status_array);
SCARLETTX_DSTORAGE_API int32_t DStorageDestroyStatusArray(uint64_t status_array);
SCARLETTX_DSTORAGE_API uint32_t DStorageIsStatusComplete(uint64_t status_array, uint32_t index);
SCARLETTX_DSTORAGE_API int32_t DStorageGetStatusHResult(uint64_t status_array, uint32_t index);
SCARLETTX_DSTORAGE_API int32_t DStorageRetrieveErrorRecord(
    uint64_t queue,
    DSTORAGE_ERROR_RECORD* out_record);

// Built-in lossless compression / decompression (LZ-RLE / GDeflate-compat framing)
SCARLETTX_DSTORAGE_API int32_t DStorageCompressBuffer(
    uint8_t format,
    const void* src,
    uint32_t src_size,
    void* dst,
    uint32_t dst_capacity,
    uint32_t* out_compressed_size);
SCARLETTX_DSTORAGE_API int32_t DStorageDecompressBuffer(
    uint8_t format,
    const void* src,
    uint32_t src_size,
    void* dst,
    uint32_t dst_capacity,
    uint32_t* out_decompressed_size);

}  // extern "C"
