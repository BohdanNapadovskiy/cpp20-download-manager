#include "core/http_transfer.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <fstream>
#include <filesystem>
#include <cstddef>
#include <charconv>
#include <string_view>
#include <curl/curl.h>

namespace dm::core {
    namespace {
        using EasyHandle = std::unique_ptr<CURL, decltype([](CURL *h) {
            curl_easy_cleanup(h);
        })>;

        enum class WriteFailure { None, Open, Write, Close, Exception };

        struct FileWriteState {
            std::filesystem::path part_path;
            std::ofstream stream{};
            std::uintmax_t bytes_transferred = 0;
            int http_status = 0;
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
                case WriteFailure::Open: return "open() failed: cannot open " + file + " for writing";
                case WriteFailure::Write: return "write() failed: cannot write to " + file;
                case WriteFailure::Close: return "close() failed: cannot flush " + file;
                case WriteFailure::Exception: return "write_callback() failed: unexpected exception writing " + file;
                case WriteFailure::None: break;
            }
            return {};
        }

        std::size_t header_callback(char *ptr, std::size_t size, std::size_t count, void *data) {
            const std::size_t total_size = size * count;
            FileWriteState *s = static_cast<FileWriteState *>(data);

            if (!s) {
                return 0;
            }
            std::string_view header_line(ptr, total_size);
            if (header_line.starts_with("HTTP/")) {
                std::size_t first_space = header_line.find(' ');
                if (first_space != std::string_view::npos) {
                    std::size_t status_pos = first_space + 1;
                    std::size_t length = (header_line.size() - status_pos);

                    if (length > 0) {
                        std::string_view status_str = header_line.substr(status_pos, length);
                        int parsed_code = 0;
                        auto res = std::from_chars(status_str.data(), status_str.data() + status_str.size(),
                                                   parsed_code);
                        if (res.ec == std::errc{}) {
                            s->http_status = parsed_code;
                        }
                    }
                }
            }
            return total_size;
        }

        std::size_t write_callback(char *ptr, std::size_t size, std::size_t nmemb, void *data) {
            FileWriteState *s = static_cast<FileWriteState *>(data);
            try {
                if (!s) {
                    return 0;
                }
                const std::size_t bytes_to_write = size * nmemb;
                if (!is_success_status(s->http_status)) {
                    return bytes_to_write;
                }
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
        long response_code = 0;
        const long second  = static_cast<long>(request.connect_timeout.count());
        FileWriteState state{.part_path = request.part_path};
        curl_easy_setopt(handle.get(), CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, error_buffer);
        curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &state);
        curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &state);
        curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_MAXREDIRS, 10L);
        curl_easy_setopt(handle.get(), CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT, second);
        const CURLcode code = curl_easy_perform(handle.get());
        auto info_code = curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &response_code);
        if (info_code != CURLE_OK) {
            response_code = 0;
        }
        const int http_status = static_cast<int>(response_code);
        if (state.stream.is_open()) {
            state.stream.close();
            if (state.stream.fail() && state.failure == WriteFailure::None) {
                state.failure = WriteFailure::Close;
            }
        }
        TransferError error = TransferError::None;
        std::string error_msg;
        if (state.failure != WriteFailure::None) {
            error = TransferError::DiskWrite;
            error_msg = describe(state.failure, state.part_path);
        } else if (code != CURLE_OK) {
            if (error_buffer[0] != '\0') {
                error_msg = error_buffer;
            } else {
                error_msg = curl_easy_strerror(code);
            }
            error = TransferError::Network;
        } else if (!is_success_status(http_status)) {
            error = TransferError::HttpStatus;
            error_msg = "download_once() failed: server responded with HTTP " + std::to_string(http_status);
        }
        return TransferResult{
            .error = error,
            .http_status_code = http_status,
            .error_message = error_msg,
            .bytes_transferred = state.bytes_transferred
        };
    }
}
