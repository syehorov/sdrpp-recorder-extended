#include <imgui.h>
#include <module.h>
#include <dsp/types.h>
#include <dsp/stream.h>
#include <dsp/bench/peak_level_meter.h>
#include <dsp/sink/handler_sink.h>
#include <dsp/routing/splitter.h>
#include <dsp/audio/volume.h>
#include <dsp/convert/stereo_to_mono.h>
#include <thread>
#include <ctime>
#include <gui/gui.h>
#include <filesystem>
#include <signal_path/signal_path.h>
#include <config.h>
#include <gui/style.h>
#include <gui/widgets/volume_meter.h>
#include <regex>
#include <gui/widgets/folder_select.h>
#include <recorder_extended_interface.h>
#include <core.h>
#include <utils/optionlist.h>
#include <utils/wav.h>
#include <radio_interface.h>
#include <lame/lame.h>
#include <vorbis/vorbisenc.h>
#include <ogg/ogg.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

#define SILENCE_LVL 10e-6

SDRPP_MOD_INFO{
    /* Name:            */ "recorder_extended",
    /* Description:     */ "Extended recorder module for SDR++",
    /* Author:          */ "Ryzerth, modified by syehorov",
    /* Version:         */ 0, 3, 1,
    /* Max instances    */ -1
};

ConfigManager config;

enum TimeZone {
    TIME_ZONE_LOCAL,
    TIME_ZONE_UTC
};

class RecorderModule : public ModuleManager::Instance {
public:
    RecorderModule(std::string name) : folderSelect("%ROOT%/recordings") {
        this->name = name;
        root = (std::string)core::args["root"];
        strcpy(nameTemplate, "$t_$f_$h-$m-$s_$d-$M-$y");
        // Define the time zones
        timezones.define("local", "Local", TIME_ZONE_LOCAL);
        timezones.define("utc", "UTC", TIME_ZONE_UTC);
        strcpy(endSuffixTemplate, "$h-$m-$s_$d-$M-$y");
        // Define recording formats
        formats.define("WAV", "WAV", "wav");
        formats.define("MP3", "MP3", "mp3");
        formats.define("OGG", "OGG", "ogg");
        // Define option lists
        containers.define("WAV", wav::FORMAT_WAV);
        // containers.define("RF64", wav::FORMAT_RF64); // Disabled for now
        sampleTypes.define(wav::SAMP_TYPE_UINT8, "Uint8", wav::SAMP_TYPE_UINT8);
        sampleTypes.define(wav::SAMP_TYPE_INT16, "Int16", wav::SAMP_TYPE_INT16);
        sampleTypes.define(wav::SAMP_TYPE_INT32, "Int32", wav::SAMP_TYPE_INT32);
        sampleTypes.define(wav::SAMP_TYPE_FLOAT32, "Float32", wav::SAMP_TYPE_FLOAT32);

        // Load default config for option lists
        timezoneId = timezones.valueId(TIME_ZONE_LOCAL);
        containerId = containers.valueId(wav::FORMAT_WAV);
        sampleTypeId = sampleTypes.valueId(wav::SAMP_TYPE_INT16);

        // Load config
        if (config.conf[name].contains("format") && formats.keyExists(config.conf[name]["format"])) {
            formatId = formats.keyId(config.conf[name]["format"]);
        }
        config.acquire();
        if (config.conf[name].contains("mode")) {
            recMode = config.conf[name]["mode"];
        }
        if (config.conf[name].contains("recPath")) {
            folderSelect.setPath(config.conf[name]["recPath"]);
        }
        if (config.conf[name].contains("timezone") && timezones.keyExists(config.conf[name]["timezone"])) {
            timezoneId = timezones.keyId(config.conf[name]["timezone"]);
        }
        if (config.conf[name].contains("container") && containers.keyExists(config.conf[name]["container"])) {
            containerId = containers.keyId(config.conf[name]["container"]);
        }
        if (config.conf[name].contains("sampleType") && sampleTypes.keyExists(config.conf[name]["sampleType"])) {
            sampleTypeId = sampleTypes.keyId(config.conf[name]["sampleType"]);
        }
        if (config.conf[name].contains("bitrate")) {
            bitrateIndex = std::clamp<int>(config.conf[name]["bitrate"], 0, 6);
        }
        if (config.conf[name].contains("samplerate")) {
            samplerateIndex = std::clamp<int>(config.conf[name]["samplerate"], 0, 4);
        }
        if (config.conf[name].contains("audioStream")) {
            selectedStreamName = config.conf[name]["audioStream"];
        }
        if (config.conf[name].contains("audioVolume")) {
            audioVolume = config.conf[name]["audioVolume"];
        }
        if (config.conf[name].contains("stereo")) {
            stereo = config.conf[name]["stereo"];
        }
        if (config.conf[name].contains("ignoreSilence")) {
            ignoreSilence = config.conf[name]["ignoreSilence"];
        }
        if (config.conf[name].contains("nameTemplate")) {
            std::string _nameTemplate = config.conf[name]["nameTemplate"];
            if (_nameTemplate.length() > sizeof(nameTemplate)-1) {
                _nameTemplate = _nameTemplate.substr(0, sizeof(nameTemplate)-1);
            }
            strcpy(nameTemplate, _nameTemplate.c_str());
        }
        if (config.conf[name].contains("addEndSuffix")) {
            addEndSuffix = config.conf[name]["addEndSuffix"];
        }
        if (config.conf[name].contains("endSuffixTemplate")) {
            std::string _endSuffixTemplate = config.conf[name]["endSuffixTemplate"];
            if (_endSuffixTemplate.length() > sizeof(endSuffixTemplate)-1) {
                _endSuffixTemplate = _endSuffixTemplate.substr(0, sizeof(endSuffixTemplate)-1);
            }
            strcpy(endSuffixTemplate, _endSuffixTemplate.c_str());
        }
        config.release();

        // Init audio path
        volume.init(NULL, audioVolume, false);
        splitter.init(&volume.out);
        splitter.bindStream(&meterStream);
        meter.init(&meterStream);
        s2m.init(&stereoStream);

        // Init sinks
        basebandSink.init(NULL, complexHandler, this);
        stereoSink.init(&stereoStream, stereoHandler, this);
        monoSink.init(&s2m.out, monoHandler, this);

        gui::menu.registerEntry(name, menuHandler, this);
        core::modComManager.registerInterface("recorder", name, moduleInterfaceHandler, this);
    }

    ~RecorderModule() {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        core::modComManager.unregisterInterface(name);
        gui::menu.removeEntry(name);
        stop();
        deselectStream();
        sigpath::sinkManager.onStreamRegistered.unbindHandler(&onStreamRegisteredHandler);
        sigpath::sinkManager.onStreamUnregister.unbindHandler(&onStreamUnregisterHandler);
        meter.stop();
    }

    void postInit() {
        // Enumerate streams
        audioStreams.clear();
        auto names = sigpath::sinkManager.getStreamNames();
        for (const auto& name : names) {
            audioStreams.define(name, name, name);
        }

        // Bind stream register/unregister handlers
        onStreamRegisteredHandler.ctx = this;
        onStreamRegisteredHandler.handler = streamRegisteredHandler;
        sigpath::sinkManager.onStreamRegistered.bindHandler(&onStreamRegisteredHandler);
        onStreamUnregisterHandler.ctx = this;
        onStreamUnregisterHandler.handler = streamUnregisterHandler;
        sigpath::sinkManager.onStreamUnregister.bindHandler(&onStreamUnregisterHandler);

        // Select the stream
        selectStream(selectedStreamName);
    }

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

    void start() {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        if (recording) { return; }

        // Configure the wav writer
        if (recMode == RECORDER_MODE_AUDIO) {
            if (selectedStreamName.empty()) { return; }
            samplerate = sigpath::sinkManager.getStreamSampleRate(selectedStreamName);
        }
        else {
            samplerate = sigpath::iqFrontEnd.getSampleRate();
        }
        writer.setFormat(containers[containerId]);
        writer.setChannels((recMode == RECORDER_MODE_AUDIO && !stereo) ? 1 : 2);
        writer.setSampleType(sampleTypes[sampleTypeId]);
        writer.setSamplerate(samplerate);

        
        if (formats.value(formatId) == "mp3") {
            this->recordingStart = std::chrono::steady_clock::now();
            lame = lame_init();
            lame_set_in_samplerate(lame, samplerate);
            lame_set_num_channels(lame, stereo ? 2 : 1);
            static const int bitrates[] = {64, 96, 128, 160, 192, 256, 320};
            static const int samplerates[] = {22050, 24000, 32000, 44100, 48000};

            lame_set_brate(lame, bitrates[bitrateIndex]);
            lame_set_in_samplerate(lame, samplerates[samplerateIndex]);
            lame_set_quality(lame, 2);

            lame_init_params(lame);

            std::string vfoName = (recMode == RECORDER_MODE_AUDIO) ? selectedStreamName : "";
            lastFilename = expandString(folderSelect.path + "/" + genFileName(nameTemplate, recMode, vfoName) + ".mp3");
            mp3File.open(lastFilename, std::ios::binary);
            if (!mp3File.is_open()) {
                flog::error("Failed to open MP3 file: {0}", lastFilename);
                return;
            }

        } else if (formats.value(formatId) == "ogg") {
            this->recordingStart = std::chrono::steady_clock::now();
            static const int bitrates[] = {64, 96, 128, 160, 192, 256, 320};
            static const int samplerates[] = {22050, 24000, 32000, 44100, 48000};
            int channels = (recMode == RECORDER_MODE_AUDIO && !stereo) ? 1 : 2;

            vorbis_info_init(&oggInfo);
            int ret = vorbis_encode_init(&oggInfo, channels, samplerates[samplerateIndex], -1, bitrates[bitrateIndex] * 1000, -1);
            if (ret) {
                flog::error("Failed to init vorbis encoder");
                vorbis_info_clear(&oggInfo);
                return;
            }

            vorbis_comment_init(&oggComment);
            vorbis_comment_add_tag(&oggComment, "ENCODER", "SDR++ recorder_extended");

            vorbis_analysis_init(&oggDsp, &oggInfo);
            vorbis_block_init(&oggDsp, &oggBlock);

            srand((unsigned)time(NULL));
            ogg_stream_init(&oggStreamState, rand());

            std::string vfoName = (recMode == RECORDER_MODE_AUDIO) ? selectedStreamName : "";
            lastFilename = expandString(folderSelect.path + "/" + genFileName(nameTemplate, recMode, vfoName) + ".ogg");
            oggFile.open(lastFilename, std::ios::binary);
            if (!oggFile.is_open()) {
                flog::error("Failed to open OGG file: {0}", lastFilename);
                vorbis_block_clear(&oggBlock);
                vorbis_dsp_clear(&oggDsp);
                vorbis_comment_clear(&oggComment);
                vorbis_info_clear(&oggInfo);
                return;
            }

            // Write the three Vorbis header packets (info, comment, codebook)
            ogg_packet header, headerComm, headerCode;
            vorbis_analysis_headerout(&oggDsp, &oggComment, &header, &headerComm, &headerCode);
            ogg_stream_packetin(&oggStreamState, &header);
            ogg_stream_packetin(&oggStreamState, &headerComm);
            ogg_stream_packetin(&oggStreamState, &headerCode);

            ogg_page page;
            while (ogg_stream_flush(&oggStreamState, &page)) {
                oggFile.write((char*)page.header, page.header_len);
                oggFile.write((char*)page.body, page.body_len);
            }

        } else {
    
            // Open file
            std::string vfoName = (recMode == RECORDER_MODE_AUDIO) ? selectedStreamName : "";
            std::string extension = ".wav";
            std::string expandedPath = expandString(folderSelect.path + "/" + genFileName(nameTemplate, recMode, vfoName) + extension);
            lastFilename = expandedPath;
            if (!writer.open(expandedPath)) {
                flog::error("Failed to open file for recording: {0}", expandedPath);
                return;
            }
        }
        // Open audio stream or baseband
        if (recMode == RECORDER_MODE_AUDIO) {
            // Start correct path depending on 
            if (stereo) {
                stereoSink.start();
            }
            else {
                s2m.start();
                monoSink.start();
            }
            splitter.bindStream(&stereoStream);
        }
        else {
            // Create and bind IQ stream
            basebandStream = new dsp::stream<dsp::complex_t>();
            basebandSink.setInput(basebandStream);
            basebandSink.start();
            sigpath::iqFrontEnd.bindIQStream(basebandStream);
        }

        recording = true;
    }

    void stop() {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        if (!recording) { return; }

        // Close audio stream or baseband
        if (recMode == RECORDER_MODE_AUDIO) {
            splitter.unbindStream(&stereoStream);
            monoSink.stop();
            stereoSink.stop();
            s2m.stop();
            
        }
        else {
            // Unbind and destroy IQ stream
            sigpath::iqFrontEnd.unbindIQStream(basebandStream);
            basebandSink.stop();
            delete basebandStream;
        }

        if (formats.value(formatId) == "mp3" && lame && mp3File.is_open()) {
            std::vector<unsigned char> flushBuf(7200);
            int flushSize = lame_encode_flush(lame, flushBuf.data(), flushBuf.size());
            if (flushSize > 0) {
                mp3File.write((char*)flushBuf.data(), flushBuf.size());
            }
            mp3File.close();
            lame_close(lame);
            lame = nullptr;
        }

        if (formats.value(formatId) == "ogg" && oggFile.is_open()) {
            // Signal end-of-stream (0 samples => EOS) and drain remaining packets
            vorbis_analysis_wrote(&oggDsp, 0);
            oggDrainPackets();

            oggFile.close();
            ogg_stream_clear(&oggStreamState);
            vorbis_block_clear(&oggBlock);
            vorbis_dsp_clear(&oggDsp);
            vorbis_comment_clear(&oggComment);
            vorbis_info_clear(&oggInfo);
        }
        // Close file
        writer.close();

        if (addEndSuffix) {
            auto endTime = std::chrono::system_clock::now();
            time_t t = std::chrono::system_clock::to_time_t(endTime);
            std::string suffix = genFileName(endSuffixTemplate, recMode, "");

            std::string newName = lastFilename.substr(0, lastFilename.find_last_of('.')) + suffix + lastFilename.substr(lastFilename.find_last_of('.'));

            try {
                std::rename(lastFilename.c_str(), newName.c_str());
                flog::info("Renamed file to: {}", newName);
            } catch (...) {
                flog::error("Failed to rename file with suffix");
            }
        }
        recording = false;
    }

private:
    static void menuHandler(void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        // Recording mode
        if (_this->recording) { style::beginDisabled(); }
        ImGui::BeginGroup();
        ImGui::Columns(2, CONCAT("RecorderModeColumns##_", _this->name), false);
        if (ImGui::RadioButton(CONCAT("Baseband##_recorder_mode_", _this->name), _this->recMode == RECORDER_MODE_BASEBAND)) {
            _this->recMode = RECORDER_MODE_BASEBAND;
            config.acquire();
            config.conf[_this->name]["mode"] = _this->recMode;
            config.release(true);
        }
        ImGui::NextColumn();
        if (ImGui::RadioButton(CONCAT("Audio##_recorder_mode_", _this->name), _this->recMode == RECORDER_MODE_AUDIO)) {
            _this->recMode = RECORDER_MODE_AUDIO;
            config.acquire();
            config.conf[_this->name]["mode"] = _this->recMode;
            config.release(true);
        }
        ImGui::Columns(1, CONCAT("EndRecorderModeColumns##_", _this->name), false);
        ImGui::EndGroup();

        // Recording path
        if (_this->folderSelect.render("##_recorder_fold_" + _this->name)) {
            if (_this->folderSelect.pathIsValid()) {
                config.acquire();
                config.conf[_this->name]["recPath"] = _this->folderSelect.path;
                config.release(true);
            }
        }

        ImGui::LeftLabel("Name template");
        ImGui::FillWidth();
        if (ImGui::InputText(CONCAT("##_recorder_name_template_", _this->name), _this->nameTemplate, 1023)) {
            config.acquire();
            config.conf[_this->name]["nameTemplate"] = _this->nameTemplate;
            config.release(true);
        }

        ImGui::LeftLabel("Time zone");
        ImGui::FillWidth();
        if (ImGui::Combo(CONCAT("##_recorder_timezone_", _this->name), &_this->timezoneId, _this->timezones.txt)) {
            config.acquire();
            config.conf[_this->name]["timezone"] = _this->timezones.key(_this->timezoneId);
            config.release(true);
        }

        ImGui::LeftLabel("Add end-time suffix");
        if (ImGui::Checkbox(CONCAT("##_recorder_add_suffix_", _this->name), &_this->addEndSuffix)) {
            config.acquire();
            config.conf[_this->name]["addEndSuffix"] = _this->addEndSuffix;
            config.release(true);
        }

        if (_this->addEndSuffix) {
            ImGui::LeftLabel("End Suffix Template");
            ImGui::FillWidth();
            if (ImGui::InputText(CONCAT("##_recorder_suffix_format_", _this->name), _this->endSuffixTemplate, 1023)) {
                config.acquire();
                config.conf[_this->name]["endSuffixTemplate"] = _this->endSuffixTemplate;
                config.release(true);
            }
        }

        ImGui::LeftLabel("Format");
        ImGui::FillWidth();
        if (ImGui::Combo(CONCAT("##_recorder_format_", _this->name), &_this->formatId, _this->formats.txt)) {
            config.acquire();
            config.conf[_this->name]["format"] = _this->formats.key(_this->formatId);
            config.release(true);
        }

        std::string fmt = _this->formats.value(_this->formatId);
        if (fmt == "mp3" || fmt == "ogg") {
            ImGui::LeftLabel("Bitrate (kbps)");
            ImGui::FillWidth();
            static const char* bitrates[] = {"64", "96", "128", "160", "192", "256", "320"};
            static int bitrateIndex = 2;
            if (ImGui::Combo(CONCAT("##_recorder_bitrate_", _this->name), &_this->bitrateIndex, bitrates, IM_ARRAYSIZE(bitrates))) {
                config.acquire();
                config.conf[_this->name]["bitrate"] = _this->bitrateIndex;
                config.release(true);
            }

            ImGui::LeftLabel("Sample rate (Hz)");
            ImGui::FillWidth();
            static const char* samplerates[] = {"22050", "24000", "32000", "44100", "48000"};
            if (ImGui::Combo(CONCAT("##_recorder_samplerate_", _this->name), &_this->samplerateIndex, samplerates, IM_ARRAYSIZE(samplerates))) {
                config.acquire();
                config.conf[_this->name]["samplerate"] = _this->samplerateIndex;
                config.release(true);
            }
        } else {
            ImGui::LeftLabel("Container");
            ImGui::FillWidth();
            if (ImGui::Combo(CONCAT("##_recorder_container_", _this->name), &_this->containerId, _this->containers.txt)) {
                config.acquire();
                config.conf[_this->name]["container"] = _this->containers.key(_this->containerId);
                config.release(true);
            }

            ImGui::LeftLabel("Sample type");
            ImGui::FillWidth();
            if (ImGui::Combo(CONCAT("##_recorder_st_", _this->name), &_this->sampleTypeId, _this->sampleTypes.txt)) {
                config.acquire();
                config.conf[_this->name]["sampleType"] = _this->sampleTypes.key(_this->sampleTypeId);
                config.release(true);
            }
        }
        if (_this->recording) { style::endDisabled(); }

        // Show additional audio options
        if (_this->recMode == RECORDER_MODE_AUDIO) {
            if (_this->recording) { style::beginDisabled(); }
            ImGui::LeftLabel("Stream");
            ImGui::FillWidth();
            if (ImGui::Combo(CONCAT("##_recorder_stream_", _this->name), &_this->streamId, _this->audioStreams.txt)) {
                _this->selectStream(_this->audioStreams.value(_this->streamId));
                config.acquire();
                config.conf[_this->name]["audioStream"] = _this->audioStreams.key(_this->streamId);
                config.release(true);
            }
            if (_this->recording) { style::endDisabled(); }

            _this->updateAudioMeter(_this->audioLvl);
            ImGui::FillWidth();
            ImGui::VolumeMeter(_this->audioLvl.l, _this->audioLvl.l, -60, 10);
            ImGui::FillWidth();
            ImGui::VolumeMeter(_this->audioLvl.r, _this->audioLvl.r, -60, 10);

            ImGui::FillWidth();
            if (ImGui::SliderFloat(CONCAT("##_recorder_vol_", _this->name), &_this->audioVolume, 0, 1, "")) {
                _this->volume.setVolume(_this->audioVolume);
                config.acquire();
                config.conf[_this->name]["audioVolume"] = _this->audioVolume;
                config.release(true);
            }

            if (_this->recording) { style::beginDisabled(); }
            if (ImGui::Checkbox(CONCAT("Stereo##_recorder_stereo_", _this->name), &_this->stereo)) {
                config.acquire();
                config.conf[_this->name]["stereo"] = _this->stereo;
                config.release(true);
            }
            if (_this->recording) { style::endDisabled(); }

            if (ImGui::Checkbox(CONCAT("Ignore silence##_recorder_ignore_silence_", _this->name), &_this->ignoreSilence)) {
                config.acquire();
                config.conf[_this->name]["ignoreSilence"] = _this->ignoreSilence;
                config.release(true);
            }
        }

        // Record button
        bool canRecord = _this->folderSelect.pathIsValid();
        if (_this->recMode == RECORDER_MODE_AUDIO) { canRecord &= !_this->selectedStreamName.empty(); }
        if (!_this->recording) {
            if (ImGui::Button(CONCAT("Record##_recorder_rec_", _this->name), ImVec2(menuWidth, 0))) {
                _this->start();
            }
            ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_Text), "Idle --:--:--");
        }
        else {
            if (ImGui::Button(CONCAT("Stop##_recorder_rec_", _this->name), ImVec2(menuWidth, 0))) {
                _this->stop();
            }
            uint64_t seconds = 0;
            if (_this->formats.value(_this->formatId) == "mp3" || _this->formats.value(_this->formatId) == "ogg") {
                auto now = std::chrono::steady_clock::now();
                seconds = std::chrono::duration_cast<std::chrono::seconds>(now - _this->recordingStart).count();
            } else {
                seconds = _this->writer.getSamplesWritten() / _this->samplerate;
            }
            time_t diff = seconds;
            tm* dtm = gmtime(&diff);

            if (_this->ignoreSilence && _this->ignoringSilence) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Paused %02d:%02d:%02d", dtm->tm_hour, dtm->tm_min, dtm->tm_sec);
            }
            else {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Recording %02d:%02d:%02d", dtm->tm_hour, dtm->tm_min, dtm->tm_sec);
            }
        }
    }

    void selectStream(std::string name) {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        deselectStream();

        if (audioStreams.empty()) {
            selectedStreamName.clear();
            return;
        }
        else if (!audioStreams.keyExists(name)) {
            selectStream(audioStreams.key(0));
            return;
        }

        audioStream = sigpath::sinkManager.bindStream(name);
        if (!audioStream) { return; }
        selectedStreamName = name;
        streamId = audioStreams.keyId(name);
        volume.setInput(audioStream);
        startAudioPath();
    }

    void deselectStream() {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        if (selectedStreamName.empty() || !audioStream) {
            selectedStreamName.clear();
            return;
        }
        if (recording && recMode == RECORDER_MODE_AUDIO) { stop(); }
        stopAudioPath();
        sigpath::sinkManager.unbindStream(selectedStreamName, audioStream);
        selectedStreamName.clear();
        audioStream = NULL;
    }

    void startAudioPath() {
        volume.start();
        splitter.start();
        meter.start();
    }

    void stopAudioPath() {
        volume.stop();
        splitter.stop();
        meter.stop();
    }

    static void streamRegisteredHandler(std::string name, void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;

        // Add new stream to the list
        _this->audioStreams.define(name, name, name);

        // If no stream is selected, select new stream. If not, update the menu ID. 
        if (_this->selectedStreamName.empty()) {
            _this->selectStream(name);
        }
        else {
            _this->streamId = _this->audioStreams.keyId(_this->selectedStreamName);
        }
    }

    static void streamUnregisterHandler(std::string name, void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;

        // Remove stream from list
        _this->audioStreams.undefineKey(name);

        // If the stream is in used, deselect it and reselect default. Otherwise, update ID.
        if (_this->selectedStreamName == name) {
            _this->selectStream("");
        }
        else {
            _this->streamId = _this->audioStreams.keyId(_this->selectedStreamName);
        }
    }

    void updateAudioMeter(dsp::stereo_t& lvl) {
        // Note: Yes, using the natural log is on purpose, it just gives a more beautiful result.
        double frameTime = 1.0 / ImGui::GetIO().Framerate;
        lvl.l = std::clamp<float>(lvl.l - (frameTime * 50.0), -90.0f, 10.0f);
        lvl.r = std::clamp<float>(lvl.r - (frameTime * 50.0), -90.0f, 10.0f);
        dsp::stereo_t rawLvl = meter.getLevel();
        meter.resetLevel();
        dsp::stereo_t dbLvl = { 10.0f * logf(rawLvl.l), 10.0f * logf(rawLvl.r) };
        if (dbLvl.l > lvl.l) { lvl.l = dbLvl.l; }
        if (dbLvl.r > lvl.r) { lvl.r = dbLvl.r; }
    }

    std::map<int, const char*> radioModeToString = {
        { RADIO_IFACE_MODE_NFM, "NFM" },
        { RADIO_IFACE_MODE_WFM, "WFM" },
        { RADIO_IFACE_MODE_AM,  "AM"  },
        { RADIO_IFACE_MODE_DSB, "DSB" },
        { RADIO_IFACE_MODE_USB, "USB" },
        { RADIO_IFACE_MODE_CW,  "CW"  },
        { RADIO_IFACE_MODE_LSB, "LSB" },
        { RADIO_IFACE_MODE_RAW, "RAW" }
    };

    std::string genFileName(std::string templ, int mode, std::string name) {
        // Get data
        time_t now = time(0);
        tm* ltm = (timezones[timezoneId] == TIME_ZONE_UTC) ? gmtime(&now) : localtime(&now);
        char buf[1024];
        double freq = gui::waterfall.getCenterFrequency();
        if (gui::waterfall.vfos.find(name) != gui::waterfall.vfos.end()) {
            freq += gui::waterfall.vfos[name]->generalOffset;
        }

        // Select the recording type string
        std::string type = (recMode == RECORDER_MODE_AUDIO) ? "audio" : "baseband";

        // Format to string
        char freqStr[128];
        char mfreqStr[128];
        char kfreqStr[128];
        char hourStr[128];
        char minStr[128];
        char secStr[128];
        char dayStr[128];
        char monStr[128];
        char yearStr[128];
        const char* modeStr = (recMode == RECORDER_MODE_AUDIO) ? "Unknown" : "IQ";
        sprintf(freqStr, "%.0lf", freq);
        sprintf(kfreqStr, "%.4lf", freq / 1000);
        sprintf(mfreqStr, "%.4lf", freq / 1000000);
        sprintf(hourStr, "%02d", ltm->tm_hour);
        sprintf(minStr, "%02d", ltm->tm_min);
        sprintf(secStr, "%02d", ltm->tm_sec);
        sprintf(dayStr, "%02d", ltm->tm_mday);
        sprintf(monStr, "%02d", ltm->tm_mon + 1);
        sprintf(yearStr, "%02d", ltm->tm_year + 1900);
        if (core::modComManager.getModuleName(name) == "radio") {
            int mode = -1;
            core::modComManager.callInterface(name, RADIO_IFACE_CMD_GET_MODE, NULL, &mode);
            if (mode >= 0) { modeStr = radioModeToString[mode]; };
        }

        // Replace in template
        templ = std::regex_replace(templ, std::regex("\\$t"), type);
        templ = std::regex_replace(templ, std::regex("\\$f"), freqStr);
        templ = std::regex_replace(templ, std::regex("\\$kf"), kfreqStr);
        templ = std::regex_replace(templ, std::regex("\\$Mf"), mfreqStr);
        templ = std::regex_replace(templ, std::regex("\\$h"), hourStr);
        templ = std::regex_replace(templ, std::regex("\\$m"), minStr);
        templ = std::regex_replace(templ, std::regex("\\$s"), secStr);
        templ = std::regex_replace(templ, std::regex("\\$d"), dayStr);
        templ = std::regex_replace(templ, std::regex("\\$M"), monStr);
        templ = std::regex_replace(templ, std::regex("\\$y"), yearStr);
        templ = std::regex_replace(templ, std::regex("\\$r"), modeStr);
        return templ;
    }

    std::string expandString(std::string input) {
        input = std::regex_replace(input, std::regex("%ROOT%"), root);
        return std::regex_replace(input, std::regex("//"), "/");
    }

    // Pulls every completed Vorbis block out of the encoder, packs it into
    // Ogg packets/pages and writes any finished pages to disk.
    void oggDrainPackets() {
        while (vorbis_analysis_blockout(&oggDsp, &oggBlock) == 1) {
            vorbis_analysis(&oggBlock, NULL);
            vorbis_bitrate_addblock(&oggBlock);

            ogg_packet packet;
            while (vorbis_bitrate_flushpacket(&oggDsp, &packet)) {
                ogg_stream_packetin(&oggStreamState, &packet);

                ogg_page page;
                while (ogg_stream_pageout(&oggStreamState, &page)) {
                    oggFile.write((char*)page.header, page.header_len);
                    oggFile.write((char*)page.body, page.body_len);
                }
            }
        }
    }

    // Accepts interleaved float samples (1 or 2 channels), de-interleaves
    // them into libvorbis' analysis buffer and drains encoded pages.
    void oggEncodeAndWrite(float* interleaved, int count, int channels) {
        float** buffer = vorbis_analysis_buffer(&oggDsp, count);
        for (int i = 0; i < count; i++) {
            for (int ch = 0; ch < channels; ch++) {
                buffer[ch][i] = interleaved[i * channels + ch];
            }
        }
        vorbis_analysis_wrote(&oggDsp, count);
        oggDrainPackets();
    }

    static void complexHandler(dsp::complex_t* data, int count, void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;
        
        if (_this->formats.value(_this->formatId) == "mp3" && _this->lame && _this->mp3File.is_open()) {
            int mp3buf_size = 1.25 * count * 2 + 7200;
            std::vector<unsigned char> mp3buf(mp3buf_size);
            int writeSize = lame_encode_buffer_interleaved_ieee_float(_this->lame, (float*)data, count, mp3buf.data(), mp3buf_size);
            if (writeSize > 0) {
                _this->mp3File.write((char*)mp3buf.data(), writeSize);
            }
            return;
        }

        if (_this->formats.value(_this->formatId) == "ogg" && _this->oggFile.is_open()) {
            _this->oggEncodeAndWrite((float*)data, count, 2);
            return;
        }

        _this->writer.write((float*)data, count);
    }

    static void stereoHandler(dsp::stereo_t* data, int count, void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;
        if (_this->ignoreSilence) {
            float absMax = 0.0f;
            float* _data = (float*)data;
            int _count = count * 2;
            for (int i = 0; i < _count; i++) {
                float val = fabsf(_data[i]);
                if (val > absMax) { absMax = val; }
            }
            _this->ignoringSilence = (absMax < SILENCE_LVL);
            if (_this->ignoringSilence) { return; }
        }
        
        if (_this->formats.value(_this->formatId) == "mp3" && _this->lame && _this->mp3File.is_open()) {
            int mp3buf_size = 1.25 * count * 2 + 7200;
            std::vector<unsigned char> mp3buf(mp3buf_size);
            int writeSize = lame_encode_buffer_interleaved_ieee_float(_this->lame, (float*)data, count, mp3buf.data(), mp3buf_size);
            if (writeSize > 0) {
                _this->mp3File.write((char*)mp3buf.data(), writeSize);
            }
            return;
        }

        if (_this->formats.value(_this->formatId) == "ogg" && _this->oggFile.is_open()) {
            _this->oggEncodeAndWrite((float*)data, count, 2);
            return;
        }

        _this->writer.write((float*)data, count);
    }

    static void monoHandler(float* data, int count, void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;
        if (_this->ignoreSilence) {
            float absMax = 0.0f;
            for (int i = 0; i < count; i++) {
                float val = fabsf(data[i]);
                if (val > absMax) { absMax = val; }
            }
            _this->ignoringSilence = (absMax < SILENCE_LVL);
            if (_this->ignoringSilence) { return; }
        }

        if (_this->formats.value(_this->formatId) == "mp3" && _this->lame && _this->mp3File.is_open()) {
            int mp3buf_size = 1.25 * count + 7200;
            std::vector<unsigned char> mp3buf(mp3buf_size);
            int writeSize = lame_encode_buffer_ieee_float(_this->lame, data, nullptr, count, mp3buf.data(), mp3buf_size);
            if (writeSize > 0) {
                _this->mp3File.write((char*)mp3buf.data(), writeSize);
            }
            return;
        }

        if (_this->formats.value(_this->formatId) == "ogg" && _this->oggFile.is_open()) {
            _this->oggEncodeAndWrite(data, count, 1);
            return;
        }
    
        _this->writer.write(data, count);
    }

    static void moduleInterfaceHandler(int code, void* in, void* out, void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;
        std::lock_guard lck(_this->recMtx);
        if (code == RECORDER_IFACE_CMD_GET_MODE) {
            int* _out = (int*)out;
            *_out = _this->recMode;
        }
        else if (code == RECORDER_IFACE_CMD_SET_MODE) {
            if (_this->recording) { return; }
            int* _in = (int*)in;
            _this->recMode = std::clamp<int>(*_in, 0, 1);
        }
        else if (code == RECORDER_IFACE_CMD_START) {
            if (!_this->recording) { _this->start(); }
        }
        else if (code == RECORDER_IFACE_CMD_STOP) {
            if (_this->recording) { _this->stop(); }
        }
    }

    std::string name;
    bool enabled = true;
    std::string root;
    char nameTemplate[1024];
    OptionList<std::string, TimeZone> timezones;
    char endSuffixTemplate[1024];
    bool addEndSuffix = false;
    std::string lastFilename;
    OptionList<std::string, wav::Format> containers;
    OptionList<std::string, std::string> formats;
    int formatId = 0;
    int bitrateIndex = 2;
    int samplerateIndex = 4;
    OptionList<int, wav::SampleType> sampleTypes;
    FolderSelect folderSelect;

    int recMode = RECORDER_MODE_AUDIO;
    int timezoneId;
    int containerId;
    int sampleTypeId;
    bool stereo = true;
    std::string selectedStreamName = "";
    float audioVolume = 1.0f;
    bool ignoreSilence = false;
    dsp::stereo_t audioLvl = { -100.0f, -100.0f };

    bool recording = false;
    bool ignoringSilence = false;
    wav::Writer writer;
    
    lame_t lame = nullptr;
    std::ofstream mp3File;
    std::chrono::steady_clock::time_point recordingStart;

    vorbis_info oggInfo;
    vorbis_comment oggComment;
    vorbis_dsp_state oggDsp;
    vorbis_block oggBlock;
    ogg_stream_state oggStreamState;
    std::ofstream oggFile;

    std::recursive_mutex recMtx;
    dsp::stream<dsp::complex_t>* basebandStream;
    dsp::stream<dsp::stereo_t> stereoStream;
    dsp::sink::Handler<dsp::complex_t> basebandSink;
    dsp::sink::Handler<dsp::stereo_t> stereoSink;
    dsp::sink::Handler<float> monoSink;

    OptionList<std::string, std::string> audioStreams;
    int streamId = 0;
    dsp::stream<dsp::stereo_t>* audioStream = NULL;
    dsp::audio::Volume volume;
    dsp::routing::Splitter<dsp::stereo_t> splitter;
    dsp::stream<dsp::stereo_t> meterStream;
    dsp::bench::PeakLevelMeter<dsp::stereo_t> meter;
    dsp::convert::StereoToMono s2m;

    uint64_t samplerate = 48000;

    EventHandler<std::string> onStreamRegisteredHandler;
    EventHandler<std::string> onStreamUnregisterHandler;

};

MOD_EXPORT void _INIT_() {
    // Create default recording directory
    std::string root = (std::string)core::args["root"];
    if (!std::filesystem::exists(root + "/recordings")) {
        flog::warn("Recordings directory does not exist, creating it");
        if (!std::filesystem::create_directory(root + "/recordings")) {
            flog::error("Could not create recordings directory");
        }
    }
    json def = json({});
    config.setPath(root + "/recorder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new RecorderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* inst) {
    delete (RecorderModule*)inst;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}