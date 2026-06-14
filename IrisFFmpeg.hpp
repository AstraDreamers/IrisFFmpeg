#pragma once

#include <cstdint>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace Iris {
    namespace Deleter {
        struct AVFormatContextDeleter {
            void operator()(AVFormatContext *format_context) const noexcept {
                AVFormatContext *raw{format_context};
                avformat_close_input(&raw);
            }
        };

        struct AVCodecContextDeleter {
            void operator()(AVCodecContext *codec_context) const noexcept {
                AVCodecContext *raw{codec_context};
                avcodec_free_context(&raw);
            }
        };

        struct AVFrameDeleter {
            void operator()(AVFrame *frame) const noexcept {
                AVFrame *raw{frame};
                av_frame_free(&raw);
            }
        };

        struct AVPacketDeleter {
            void operator()(AVPacket *packet) const noexcept {
                AVPacket *raw{packet};
                av_packet_free(&raw);
            }
        };

        struct SwsContextDeleter {
            void operator()(SwsContext *sws_context) const noexcept {
                sws_freeContext(sws_context);
            }
        };

        struct SwrContextDeleter {
            void operator()(SwrContext *swr_context) const noexcept {
                SwrContext *raw{swr_context};
                swr_free(&raw);
            }
        };
    } // namespace Deleter

    enum class PacketType {
        Audio,
        Video,
        EndOfFile,
        Error,
    };

    using RAIIAVFormatContext = std::unique_ptr<AVFormatContext, Deleter::AVFormatContextDeleter>;
    using RAIIAVCodecContext = std::unique_ptr<AVCodecContext, Deleter::AVCodecContextDeleter>;
    using RAIIAVFrame = std::unique_ptr<AVFrame, Deleter::AVFrameDeleter>;
    using RAIIAVPacket = std::unique_ptr<AVPacket, Deleter::AVPacketDeleter>;
    using RAIISwsContext = std::unique_ptr<SwsContext, Deleter::SwsContextDeleter>;
    using RAIISwrContext = std::unique_ptr<SwrContext, Deleter::SwrContextDeleter>;

    class IrisFFmpeg final {
      public:
        IrisFFmpeg() = default;
        ~IrisFFmpeg() = default;

        IrisFFmpeg(const IrisFFmpeg &) = delete;
        IrisFFmpeg &operator=(const IrisFFmpeg &) = delete;
        IrisFFmpeg(IrisFFmpeg &&) noexcept = default;
        IrisFFmpeg &operator=(IrisFFmpeg &&) noexcept = default;

        [[nodiscard]] bool openFromFile(const std::string &path) {
            try {

                reset();

                AVFormatContext *raw_format_context{nullptr};
                if (avformat_open_input(&raw_format_context, path.data(), nullptr, nullptr) < 0) {
                    throw std::runtime_error("Failure in opening file " + path);
                }
                m_format_context.reset(raw_format_context);

                if (avformat_find_stream_info(m_format_context.get(), nullptr) < 0) {
                    throw std::runtime_error("Failure in finding stream information in file " + path);
                }

                m_stream_index_video = av_find_best_stream(m_format_context.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
                m_stream_index_audio = av_find_best_stream(m_format_context.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

                if (m_stream_index_video >= 0) {
                    AVStream *stream = m_format_context->streams[m_stream_index_video];
                    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
                    if (!codec) {
                        throw std::runtime_error("Unknown video codec.");
                    }
                    m_codec_context_video.reset(avcodec_alloc_context3(codec));
                    if (!m_codec_context_video) {
                        throw std::runtime_error("Failure in allocating video codec context");
                    }
                    if (avcodec_parameters_to_context(m_codec_context_video.get(), stream->codecpar) < 0) {
                        throw std::runtime_error("Failure in copying video codec parameters to context.");
                    }
                    m_codec_context_video->thread_count = 0;
                    if (avcodec_open2(m_codec_context_video.get(), codec, nullptr) < 0) {
                        throw std::runtime_error("Failure in opening video codec.");
                    }
                    m_frame_video.reset(av_frame_alloc());
                    if (!m_frame_video) {
                        throw std::runtime_error("Failure in allocating video frame.");
                    }
                }

                if (m_stream_index_audio >= 0) {
                    AVStream *stream = m_format_context->streams[m_stream_index_audio];
                    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
                    if (!codec) {
                        throw std::runtime_error("Unknown audio codec.");
                    }
                    m_codec_context_audio.reset(avcodec_alloc_context3(codec));
                    if (!m_codec_context_audio) {
                        throw std::runtime_error("Failure in allocating audio codec context");
                    }
                    if (avcodec_parameters_to_context(m_codec_context_audio.get(), stream->codecpar) < 0) {
                        throw std::runtime_error("Failure in copying audio codec parameters to context.");
                    }
                    m_codec_context_audio->thread_count = 0;
                    if (avcodec_open2(m_codec_context_audio.get(), codec, nullptr) < 0) {
                        throw std::runtime_error("Failure in opening audio codec.");
                    }
                    m_frame_audio.reset(av_frame_alloc());
                    if (!m_frame_audio) {
                        throw std::runtime_error("Failure in allocating audio frame.");
                    }
                }

                m_packet.reset(av_packet_alloc());
                if (!m_packet) {
                    throw std::runtime_error("Failure in allocating packet");
                }

                return true;

            } catch (const std::runtime_error &e) {
                reset();
                std::println(stderr, "[FFmpeg] {}", e.what());
                return false;
            }
        }

        [[nodiscard]] bool haveVideoStream() const noexcept {
            return m_stream_index_video >= 0;
        }

        [[nodiscard]] bool haveAudioStream() const noexcept {
            return m_stream_index_audio >= 0;
        }

        void reset() noexcept {
            m_packet.reset();
            m_frame_video.reset();
            m_frame_audio.reset();
            m_codec_context_video.reset();
            m_codec_context_audio.reset();
            m_format_context.reset();

            m_stream_index_video = -1;
            m_stream_index_audio = -1;
        }

      private:
        int32_t m_stream_index_video{-1};
        int32_t m_stream_index_audio{-1};

        RAIIAVPacket m_packet{nullptr};
        RAIIAVFrame m_frame_video{nullptr};
        RAIIAVFrame m_frame_audio{nullptr};
        RAIIAVCodecContext m_codec_context_video{nullptr};
        RAIIAVCodecContext m_codec_context_audio{nullptr};
        RAIIAVFormatContext m_format_context{nullptr};
    };
} // namespace Iris