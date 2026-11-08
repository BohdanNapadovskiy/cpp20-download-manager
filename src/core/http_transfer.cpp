#include "core/http_transfer.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <fstream>
#include <filesystem>
#include <cstddef>
#include <curl/curl.h>

namespace dm::core {
    namespace {
        using EasyHandle = std::unique_ptr<CURL, decltype([](CURL *h) {
            curl_easy_cleanup(h);
        })>;

        // Which file operation failed. The callback only records it: building the message
        // allocates, and nothing may throw while curl is inside write_callback.
        enum class WriteFailure { None, Open, Write, Close, Exception };

        struct FileWriteState {
            std::filesystem::path part_path;
            std::ofstream stream{};
            std::uintmax_t bytes_transferred = 0;
            WriteFailure failure = WriteFailure::None;
        };

        // path::string() converts to the ANSI code page on Windows and mangles non-ASCII names.
        std::string to_utf8(const std::filesystem::path &path) {
            const std::u8string utf8 = path.u8string();
            return {reinterpret_cast<const char *>(utf8.data()), utf8.size()};
        }

        // Message in the "<function>() failed: <reason>" format from README.
        std::string describe(WriteFailure failure, const std::filesystem::path &path) {
            const std::string file = to_utf8(path);
            switch (failure) {
                case WriteFailure::Open:      return "open() failed: cannot open " + file + " for writing";
                case WriteFailure::Write:     return "write() failed: cannot write to " + file;
                case WriteFailure::Close:     return "close() failed: cannot flush " + file;
                case WriteFailure::Exception: return "write_callback() failed: unexpected exception writing " + file;
                case WriteFailure::None:      break;
            }
            return {};
        }

        std::size_t write_callback(char *ptr, std::size_t size, std::size_t nmemb, void *data) {
            FileWriteState *s = static_cast<FileWriteState *>(data);
            try {
                if (!s) {
                    return 0;
                }
                const std::size_t bytes_to_write = size * nmemb;
                if (!s->stream.is_open()) {
                    s->stream.open(s->part_path, std::ios::out | std::ios::binary);
                    if (!s->stream) {
                        s->failure = WriteFailure::Open;
                        return 0;
                    }
                }
                s->stream.write(ptr, static_cast<std::streamsize>(bytes_to_write));
                if (!s->stream) {
                    s->failure = WriteFailure::Write;
                    return 0;
                }
                s->bytes_transferred += bytes_to_write;
                return bytes_to_write;
            } catch (...) {
                if (s != nullptr) {
                    s->failure = WriteFailure::Exception;
                }
                return 0;
            }
        }

        EasyHandle create_easy_handle() {
            CURL *handle = curl_easy_init();
            if (handle == nullptr) {
                throw std::runtime_error("curl_easy_init() failed: returned nullptr");
            }
            return EasyHandle{handle};
        }
    }

    TransferResult download_once(const TransferRequest &request) {
        EasyHandle handle = create_easy_handle();
        char error_buffer[CURL_ERROR_SIZE];
        error_buffer[0] = '\0';
        FileWriteState state{.part_path = request.part_path};
        curl_easy_setopt(handle.get(), CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, error_buffer);
        curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &state);
        const CURLcode code = curl_easy_perform(handle.get());
        if (state.stream.is_open()) {
            state.stream.close();
            if (state.stream.fail() && state.failure == WriteFailure::None) {
                state.failure = WriteFailure::Close;
            }
        }
        if (state.failure != WriteFailure::None) {
            return TransferResult{
                .error = TransferError::DiskWrite,
                .error_message = describe(state.failure, state.part_path),
                .bytes_transferred = state.bytes_transferred
            };
        }
        if (code != CURLE_OK) {
            std::string error_msg;
            if (error_buffer[0] != '\0') {
                error_msg = error_buffer;
            } else {
                error_msg = curl_easy_strerror(code);
            }
            return TransferResult{
                .error = TransferError::Network,
                .error_message = error_msg,
                .bytes_transferred = state.bytes_transferred
            };
        }
        return TransferResult{
            .error = TransferError::None,
            .bytes_transferred = state.bytes_transferred
        };
    }
}
