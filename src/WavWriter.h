#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>

// The course SDK delivers 16000 Hz, mono, signed 16-bit little-endian PCM.
class WavWriter {
    std::ofstream out_;
    uint32_t bytes_ = 0;
    void u16(uint16_t x) { char b[2]={char(x),char(x>>8)}; out_.write(b,2); }
    void u32(uint32_t x) { char b[4]={char(x),char(x>>8),char(x>>16),char(x>>24)}; out_.write(b,4); }
    void header() {
        out_.seekp(0); out_.write("RIFF",4); u32(bytes_+36); out_.write("WAVEfmt ",8);
        u32(16); u16(1); u16(1); u32(16000); u32(32000); u16(2); u16(16);
        out_.write("data",4); u32(bytes_);
    }
public:
    explicit WavWriter(const std::filesystem::path& path):out_(path,std::ios::binary|std::ios::trunc) {
        if(!out_) throw std::runtime_error("Cannot create WAV file");
        header();
    }
    WavWriter(const WavWriter&)=delete;
    WavWriter& operator=(const WavWriter&)=delete;
    void append(const uint8_t* data,size_t count) {
        if(!data || count%2 || count>UINT32_C(0x7fffffff)-bytes_)
            throw std::runtime_error("Invalid PCM size or 2 GiB WAV limit reached");
        out_.write(reinterpret_cast<const char*>(data),static_cast<std::streamsize>(count));
        if(!out_) throw std::runtime_error("WAV write failed");
        bytes_+=static_cast<uint32_t>(count);
    }
    void finish() {
        if(!out_.is_open()) return;
        header(); out_.flush();
        if(!out_) throw std::runtime_error("WAV finalization failed");
        out_.close();
    }
    ~WavWriter() { try { finish(); } catch(...) {} }
};
