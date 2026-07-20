#include "media_source.h"

// stb_image is vendored in llama.cpp
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propsys.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
#endif

// ═══════════════════════════════════════════════════════════════
// ImageSource
// ═══════════════════════════════════════════════════════════════

ImageSource::ImageSource(const std::string & filepath)
    : _filepath(filepath) {}

ImageSource::~ImageSource() { close(); }

bool ImageSource::open() {
    if (_opened) return true;

    int w = 0, h = 0, c = 0;
    unsigned char * pixels = stbi_load(_filepath.c_str(), &w, &h, &c, 3);
    if (!pixels) {
        fprintf(stderr, "ImageSource: failed to load %s: %s\n",
                _filepath.c_str(), stbi_failure_reason());
        return false;
    }

    _frame.type     = MediaType::Image;
    _frame.width    = w;
    _frame.height   = h;
    _frame.channels = 3;
    _frame.data.assign(pixels, pixels + (size_t)(w * h * 3));
    stbi_image_free(pixels);

    if (_filepath.find(".png") != std::string::npos ||
        _filepath.find(".PNG") != std::string::npos)
        _frame.mime_type = "image/png";
    else if (_filepath.find(".jpg") != std::string::npos ||
             _filepath.find(".JPG") != std::string::npos ||
             _filepath.find(".jpeg") != std::string::npos ||
             _filepath.find(".JPEG") != std::string::npos)
        _frame.mime_type = "image/jpeg";
    else if (_filepath.find(".webp") != std::string::npos ||
             _filepath.find(".WEBP") != std::string::npos)
        _frame.mime_type = "image/webp";
    else if (_filepath.find(".bmp") != std::string::npos ||
             _filepath.find(".BMP") != std::string::npos)
        _frame.mime_type = "image/bmp";

    _opened   = true;
    _consumed = false;
    return true;
}

void ImageSource::close() {
    _frame = MediaFrame{};
    _opened   = false;
    _consumed = false;
}

MediaFrame ImageSource::next_frame() {
    if (!_opened || _consumed) return MediaFrame{};
    _consumed = true;
    return _frame;
}

bool ImageSource::has_next() const {
    return _opened && !_consumed;
}

std::string ImageSource::description() const {
    return std::string("Image: ") + _filepath;
}

// ═══════════════════════════════════════════════════════════════
// VideoSource — Windows Media Foundation
// ═══════════════════════════════════════════════════════════════

struct VideoSource::Impl {
#ifdef _WIN32
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType>    native_type;
    int64_t total_frames  = 0;
    int64_t frame_count   = 0;
    bool    first_sample  = true;
    bool    mf_started    = false;

    // Cached frame info
    int out_width  = 0;
    int out_height = 0;
#endif
};

VideoSource::VideoSource(const std::string & filepath)
    : _filepath(filepath) {}

VideoSource::~VideoSource() { close(); }

#ifdef _WIN32
// Helper: convert MF sample to RGB MediaFrame
static MediaFrame sample_to_frame(IMFSample * sample, int width, int height) {
    MediaFrame frame;
    frame.type     = MediaType::Video;
    frame.width    = width;
    frame.height   = height;
    frame.channels = 3;
    frame.mime_type = "image/bmp";  // raw RGB data

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
        return {};

    BYTE * data = nullptr;
    DWORD len   = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &len)))
        return {};

    // Media Foundation delivers RGB32 by default (4 bytes/pixel).
    // Convert RGB32 → RGB24 (strip the alpha channel).
    size_t src_pixels = (size_t)width * height;
    if (len >= src_pixels * 4) {
        frame.data.resize(src_pixels * 3);
        for (size_t i = 0; i < src_pixels; ++i) {
            frame.data[i * 3 + 0] = data[i * 4 + 0]; // R
            frame.data[i * 3 + 1] = data[i * 4 + 1]; // G
            frame.data[i * 3 + 2] = data[i * 4 + 2]; // B
        }
    } else {
        frame.data.assign(data, data + len);
    }

    // Get timestamp
    LONGLONG duration = 0, hns = 0;
    if (SUCCEEDED(sample->GetSampleTime(&hns))) {
        frame.timestamp_us = (int64_t)(hns / 10); // 100-ns → µs
    }

    buffer->Unlock();
    return frame;
}
#endif

bool VideoSource::open() {
    if (_opened) return true;
    if (!_impl) _impl = new Impl;

#ifdef _WIN32
    // Initialize COM (safe to call multiple times in STA)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "VideoSource: CoInitializeEx failed (0x%08lx)\n", hr);
        return false;
    }

    // Start Media Foundation
    hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) {
        fprintf(stderr, "VideoSource: MFStartup failed (0x%08lx)\n", hr);
        return false;
    }
    _impl->mf_started = true;

    // Create source reader
    hr = MFCreateSourceReaderFromURL(
        _filepath.c_str(), nullptr, &_impl->reader);
    if (FAILED(hr)) {
        fprintf(stderr, "VideoSource: failed to open %s (0x%08lx)\n",
                _filepath.c_str(), hr);
        return false;
    }

    // Set output type to RGB32 on the video stream
    ComPtr<IMFMediaType> media_type;
    hr = MFCreateMediaType(&media_type);
    if (FAILED(hr)) return false;

    hr = media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) return false;
    hr = media_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(hr)) return false;

    hr = _impl->reader->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, media_type.Get());
    if (FAILED(hr)) {
        fprintf(stderr, "VideoSource: SetCurrentMediaType failed (0x%08lx)\n", hr);
        return false;
    }

    // Get the negotiated output dimensions
    ComPtr<IMFMediaType> output_type;
    hr = _impl->reader->GetCurrentMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, &output_type);
    if (SUCCEEDED(hr)) {
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, &w, &h);
        _impl->out_width  = (int)w;
        _impl->out_height = (int)h;

        // Get total duration for frame count estimate
        PROPVARIANT var;
        PropVariantInit(&var);
        ComPtr<IMFMediaSource> source;
        if (SUCCEEDED(_impl->reader->GetServiceForStream(
                MF_SOURCE_READER_MEDIASOURCE, GUID_NULL,
                IID_PPV_ARGS(&source)))) {
            if (SUCCEEDED(source->GetPresentationDescriptor(&var))) {
                // Use duration for metadata
            }
            PropVariantClear(&var);
        }
    }

    // Prefetch frames into the queue
    _impl->frame_count = 0;
    while (true) {
        DWORD flags      = 0;
        DWORD stream_idx = 0;
        ComPtr<IMFSample> sample;
        hr = _impl->reader->ReadSample(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM,
            0, &stream_idx, &flags, nullptr, &sample);

        if (FAILED(hr)) break;

        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            _eof = true;
            break;
        }

        if (sample) {
            MediaFrame frame = sample_to_frame(
                sample.Get(), _impl->out_width, _impl->out_height);
            if (!frame.empty()) {
                _frame_queue.push(std::move(frame));
            }
        }
    }

    _opened = true;
    return true;

#else
    fprintf(stderr, "VideoSource: only supported on Windows\n");
    return false;
#endif
}

void VideoSource::close() {
    _opened = false;
    _eof    = false;
    while (!_frame_queue.empty()) _frame_queue.pop();

    if (_impl) {
#ifdef _WIN32
        _impl->reader.Reset();
        if (_impl->mf_started) {
            MFShutdown();
            _impl->mf_started = false;
        }
        CoUninitialize();
#endif
        delete _impl;
        _impl = nullptr;
    }
}

MediaFrame VideoSource::next_frame() {
    if (!_opened || _frame_queue.empty()) return MediaFrame{};
    MediaFrame frame = std::move(_frame_queue.front());
    _frame_queue.pop();
    return frame;
}

bool VideoSource::has_next() const {
    return _opened && !_frame_queue.empty();
}

std::string VideoSource::description() const {
    return std::string("Video: ") + _filepath;
}

// ═══════════════════════════════════════════════════════════════
// CameraSource — Windows Media Foundation (video capture)
// ═══════════════════════════════════════════════════════════════

struct CameraSource::Impl {
#ifdef _WIN32
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaSource>  media_source;
    bool mf_started  = false;

    int out_width  = 0;
    int out_height = 0;
#endif
};

CameraSource::CameraSource(int device_id)
    : _device_id(device_id) {}

CameraSource::~CameraSource() { close(); }

bool CameraSource::open() {
    if (_opened) return true;
    if (!_impl) _impl = new Impl;

#ifdef _WIN32
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;

    hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) return false;
    _impl->mf_started = true;

    // Enumerate video capture devices
    ComPtr<IMFAttributes> attributes;
    hr = MFCreateAttributes(&attributes, 1);
    if (FAILED(hr)) return false;

    hr = attributes->SetGUID(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) return false;

    IMFActivate ** devices = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(attributes.Get(), &devices, &count);
    if (FAILED(hr) || count == 0) {
        fprintf(stderr, "CameraSource: no capture devices found\n");
        if (devices) CoTaskMemFree(devices);
        return false;
    }

    // Select device by index
    UINT32 idx = (UINT32)_device_id;
    if (idx >= count) idx = 0;

    hr = devices[idx]->ActivateObject(
        IID_PPV_ARGS(&_impl->media_source));
    for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
    CoTaskMemFree(devices);

    if (FAILED(hr) || !_impl->media_source) return false;

    // Create source reader from the capture device
    hr = MFCreateSourceReaderFromMediaSource(
        _impl->media_source.Get(), nullptr, &_impl->reader);
    if (FAILED(hr)) return false;

    // Set output type to RGB32
    ComPtr<IMFMediaType> media_type;
    hr = MFCreateMediaType(&media_type);
    if (FAILED(hr)) return false;

    hr = media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) return false;
    hr = media_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(hr)) return false;

    hr = _impl->reader->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, media_type.Get());
    if (FAILED(hr)) return false;

    // Get dimensions
    ComPtr<IMFMediaType> output_type;
    hr = _impl->reader->GetCurrentMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, &output_type);
    if (SUCCEEDED(hr)) {
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, &w, &h);
        _impl->out_width  = (int)w;
        _impl->out_height = (int)h;
    }

    _opened = true;
    return true;

#else
    fprintf(stderr, "CameraSource: only supported on Windows\n");
    return false;
#endif
}

void CameraSource::close() {
    _opened = false;
    _eof    = false;
    while (!_frame_queue.empty()) _frame_queue.pop();

    if (_impl) {
#ifdef _WIN32
        _impl->reader.Reset();
        _impl->media_source.Reset();
        if (_impl->mf_started) {
            MFShutdown();
            _impl->mf_started = false;
        }
        CoUninitialize();
#endif
        delete _impl;
        _impl = nullptr;
    }
}

MediaFrame CameraSource::next_frame() {
    if (!_opened) return MediaFrame{};

    // Try the queue first
    if (!_frame_queue.empty()) {
        MediaFrame frame = std::move(_frame_queue.front());
        _frame_queue.pop();
        return frame;
    }

#ifdef _WIN32
    // Capture one fresh frame
    if (!_impl || !_impl->reader) return MediaFrame{};

    DWORD flags      = 0;
    DWORD stream_idx = 0;
    ComPtr<IMFSample> sample;
    HRESULT hr = _impl->reader->ReadSample(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM,
        0, &stream_idx, &flags, nullptr, &sample);

    if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
        _eof = true;
        return MediaFrame{};
    }

    if (!sample) return MediaFrame{};

    MediaFrame frame = sample_to_frame(
        sample.Get(), _impl->out_width, _impl->out_height);
    return frame;

#else
    return MediaFrame{};
#endif
}

bool CameraSource::has_next() const {
    return _opened && !_eof;
}

std::string CameraSource::description() const {
    return std::string("Camera #") + std::to_string(_device_id);
}

// ═══════════════════════════════════════════════════════════════
// DataStreamSource — CSV / JSON Lines reader
// ═══════════════════════════════════════════════════════════════

DataStreamSource::DataStreamSource(const std::string & filepath, int batch_size)
    : _filepath(filepath), _batch_size(batch_size) {}

DataStreamSource::~DataStreamSource() { close(); }

static std::string detect_format(const std::string & path) {
    // Check extension
    std::string lower = path;
    for (auto & c : lower) c = (char)std::tolower((unsigned char)c);

    if (lower.find(".csv")  != std::string::npos) return "csv";
    if (lower.find(".json") != std::string::npos ||
        lower.find(".jsonl") != std::string::npos ||
        lower.find(".ndjson") != std::string::npos) return "json";
    return "csv"; // default
}

// Build a human-readable textual representation of a CSV row
static std::string format_csv_row(const std::string & header_line,
                                  const std::string & row_line) {
    auto split = [](const std::string & s) {
        std::vector<std::string> fields;
        std::string cur;
        bool in_quotes = false;
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (c == '"') {
                in_quotes = !in_quotes;
            } else if (c == ',' && !in_quotes) {
                // Trim whitespace
                size_t start = cur.find_first_not_of(" \t\r");
                size_t end   = cur.find_last_not_of(" \t\r");
                fields.push_back(start != std::string::npos
                    ? cur.substr(start, end - start + 1) : "");
                cur.clear();
            } else {
                cur += c;
            }
        }
        size_t start = cur.find_first_not_of(" \t\r");
        size_t end   = cur.find_last_not_of(" \t\r");
        fields.push_back(start != std::string::npos
            ? cur.substr(start, end - start + 1) : "");
        return fields;
    };

    auto headers = split(header_line);
    auto values  = split(row_line);

    std::string result;
    for (size_t i = 0; i < headers.size() && i < values.size(); ++i) {
        if (!result.empty()) result += ", ";
        result += headers[i] + ": " + values[i];
    }
    return result;
}

bool DataStreamSource::open() {
    if (_opened) return true;

    _file.open(_filepath, std::ios::in);
    if (!_file.is_open()) {
        fprintf(stderr, "DataStreamSource: failed to open %s\n",
                _filepath.c_str());
        return false;
    }

    _delimiter   = detect_format(_filepath);
    _header_read = false;
    _header_line.clear();
    _opened = true;
    return true;
}

void DataStreamSource::close() {
    _opened = false;
    if (_file.is_open()) _file.close();
}

MediaFrame DataStreamSource::next_frame() {
    if (!_opened || !_file.is_open()) return MediaFrame{};

    // Peek at next char to detect EOF
    if (_file.peek() == EOF) {
        return MediaFrame{};
    }

    std::string batch_text;
    int lines_read = 0;

    if (_delimiter == "json") {
        // JSON Lines: read batch_size lines
        std::string line;
        while (lines_read < _batch_size && std::getline(_file, line)) {
            if (!line.empty()) {
                if (!batch_text.empty()) batch_text += "\n";
                batch_text += line;
                ++lines_read;
            }
        }
    } else {
        // CSV: read header + batch_size data rows
        if (!_header_read) {
            std::getline(_file, _header_line);
            while (!_header_line.empty() &&
                   (_header_line.front() == '\xEF' ||
                    _header_line.front() == '\xBB' ||
                    _header_line.front() == '\xBF' ||
                    _header_line.front() == ' ' ||
                    _header_line.front() == '\t'))
                _header_line.erase(_header_line.begin());
            _header_read = true;
        }

        std::string line;
        while (lines_read < _batch_size && std::getline(_file, line)) {
            if (!line.empty() && !_header_line.empty()) {
                std::string formatted = format_csv_row(_header_line, line);
                if (!batch_text.empty()) batch_text += "\n";
                batch_text += formatted;
                ++lines_read;
            }
        }
    }

    if (batch_text.empty()) return MediaFrame{};

    MediaFrame frame;
    frame.type       = MediaType::Data;
    frame.mime_type  = (_delimiter == "json") ? "application/json" : "text/csv";
    frame.annotation = batch_text;

    // Store the text in data field (as UTF-8)
    frame.data.assign(batch_text.begin(), batch_text.end());

    return frame;
}

bool DataStreamSource::has_next() const {
    if (!_opened || !_file.is_open()) return false;
    return _file.peek() != EOF;
}

std::string DataStreamSource::description() const {
    return std::string("Data: ") + _filepath +
           " (batch=" + std::to_string(_batch_size) + ")";
}

// ═══════════════════════════════════════════════════════════════
// TextStreamSource — large text file chunk reader
// ═══════════════════════════════════════════════════════════════

TextStreamSource::TextStreamSource(const std::string & filepath, int chunk_tokens)
    : _filepath(filepath), _chunk_tokens(chunk_tokens) {}

TextStreamSource::~TextStreamSource() { close(); }

// Rough token estimate: ~4 characters per token for English text
static size_t chars_for_tokens(int n_tokens) {
    return (size_t)n_tokens * 4;
}

bool TextStreamSource::open() {
    if (_opened) return true;

    _file.open(_filepath, std::ios::in);
    if (!_file.is_open()) {
        fprintf(stderr, "TextStreamSource: failed to open %s\n",
                _filepath.c_str());
        return false;
    }

    _eof    = false;
    _opened = true;
    return true;
}

void TextStreamSource::close() {
    _opened = false;
    _eof    = false;
    if (_file.is_open()) _file.close();
}

MediaFrame TextStreamSource::next_frame() {
    if (!_opened || !_file.is_open() || _eof) return MediaFrame{};

    size_t target_chars = chars_for_tokens(_chunk_tokens);
    std::string chunk;
    chunk.reserve(target_chars + 256);

    std::string line;
    bool first_line = true;

    while (chunk.size() < target_chars && std::getline(_file, line)) {
        if (!first_line) chunk += "\n";
        chunk += line;
        first_line = false;
    }

    if (chunk.empty()) {
        _eof = true;
        return MediaFrame{};
    }

    MediaFrame frame;
    frame.type      = MediaType::Text;
    frame.mime_type = "text/plain";

    // Estimate token count
    size_t char_count = chunk.size();
    int estimated_tokens = (int)(char_count / 4);
    frame.annotation = std::to_string(estimated_tokens) + " tokens";

    frame.data.assign(chunk.begin(), chunk.end());

    return frame;
}

bool TextStreamSource::has_next() const {
    return _opened && !_eof && _file.is_open() && _file.peek() != EOF;
}

std::string TextStreamSource::description() const {
    return std::string("Text: ") + _filepath +
           " (chunk=" + std::to_string(_chunk_tokens) + " tokens)";
}
