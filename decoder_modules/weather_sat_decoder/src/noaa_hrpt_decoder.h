#pragma once
#include <sat_decoder.h>
#include "hrpt_demod.h"
#include "noaa_hrpt.h"
#include <dsp/routing/splitter.h>
#include <dsp/buffer/reshaper.h>
#include <dsp/sink/handler_sink.h>
#include <gui/widgets/constellation_diagram.h>
#include <gui/widgets/line_push_image.h>
#include <gui/gui.h>

class NOAAHRPTDecoder : public SatDecoder {
public:
    NOAAHRPTDecoder(VFOManager::VFO* vfo, std::string name)
        : avhrrRGBImage(noaa::HRPT_AVHRR_PIXELS, 256),
          avhrr1Image(noaa::HRPT_AVHRR_PIXELS, 256),
          avhrr2Image(noaa::HRPT_AVHRR_PIXELS, 256),
          avhrr3Image(noaa::HRPT_AVHRR_PIXELS, 256),
          avhrr4Image(noaa::HRPT_AVHRR_PIXELS, 256),
          avhrr5Image(noaa::HRPT_AVHRR_PIXELS, 256) {
        _vfo = vfo;
        _name = name;

        // Allocate frame buffers
        manchFrame = new uint8_t[noaa::HRPT_MANCH_FRAME_BITS];
        decodedFrame = new uint8_t[noaa::HRPT_FRAME_BITS];

        // Generate Manchester-encoded sync word for correlation
        noaa::getManchesterSyncBits(manchSyncWord);

        // DSP chain: demod -> splitter -> [vis constellation + data handler]
        demod.init(vfo->output, noaa::HRPT_SYMBOL_RATE, noaa::HRPT_VFO_SR,
                   33, 0.6, 0.1, 0.005, 1e-6, 0.01);

        split.init(&demod.out);
        split.bindStream(&visStream);
        split.bindStream(&dataStream);

        reshape.init(&visStream, 1024, (noaa::HRPT_SYMBOL_RATE / 30) - 1024);
        visSink.init(&reshape.out, visHandler, this);
        dataSink.init(&dataStream, dataHandler, this);
    }

    ~NOAAHRPTDecoder() {
        delete[] manchFrame;
        delete[] decodedFrame;
    }

    void select() override {
        _vfo->setSampleRate(noaa::HRPT_VFO_SR, noaa::HRPT_VFO_BW);
        _vfo->setReference(ImGui::WaterfallVFO::REF_CENTER);
        _vfo->setBandwidthLimits(noaa::HRPT_VFO_BW, noaa::HRPT_VFO_BW, true);
    }

    void start() override {
        synced = false;
        manchFramePos = 0;
        syncWindowLen = 0;

        demod.start();
        split.start();
        reshape.start();
        visSink.start();
        dataSink.start();
    }

    void stop() override {
        demod.stop();
        split.stop();
        reshape.stop();
        visSink.stop();
        dataSink.stop();
    }

    void setVFO(VFOManager::VFO* vfo) override {
        _vfo = vfo;
        demod.setInput(_vfo->output);
    }

    bool canRecord() override { return false; }

    void drawMenu(float menuWidth) override {
        ImGui::SetNextItemWidth(menuWidth);
        constDiag.draw();

        gui::mainWindow.lockWaterfallControls = showWindow;
        if (showWindow) {
            ImGui::Begin("NOAA HRPT Decoder");
            ImGui::BeginTabBar("NOAAHRPTTabs");

            if (ImGui::BeginTabItem("AVHRR RGB(221)")) {
                ImGui::BeginChild("AVHRRRGBChild");
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                avhrrRGBImage.draw();
                ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            drawChannelTab("AVHRR 1", "AVHRR1Child", avhrr1Image);
            drawChannelTab("AVHRR 2", "AVHRR2Child", avhrr2Image);
            drawChannelTab("AVHRR 3", "AVHRR3Child", avhrr3Image);
            drawChannelTab("AVHRR 4", "AVHRR4Child", avhrr4Image);
            drawChannelTab("AVHRR 5", "AVHRR5Child", avhrr5Image);

            ImGui::EndTabBar();
            ImGui::End();
        }

        ImGui::Checkbox("Show Image", &showWindow);
    }

private:
    static void drawChannelTab(const char* label, const char* childId, ImGui::LinePushImage& img) {
        if (ImGui::BeginTabItem(label)) {
            ImGui::BeginChild(childId);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            img.draw();
            ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
    }

    static void visHandler(dsp::complex_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
        dsp::complex_t* buf = _this->constDiag.acquireBuffer();
        memcpy(buf, data, 1024 * sizeof(dsp::complex_t));
        _this->constDiag.releaseBuffer();
    }

    static void dataHandler(dsp::complex_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
        // BPSK: symbol information is in the real part
        for (int i = 0; i < count; i++) {
            uint8_t bit = data[i].re > 0.0f ? 1 : 0;
            _this->processBit(bit);
        }
    }

    void processBit(uint8_t bit) {
        if (synced) {
            // Collecting Manchester frame bits
            manchFrame[manchFramePos++] = bit;
            if (manchFramePos >= noaa::HRPT_MANCH_FRAME_BITS) {
                // Complete frame received, decode and process
                int decodedLen = noaa::manchesterDecode(manchFrame, decodedFrame, noaa::HRPT_MANCH_FRAME_BITS);
                if (decodedLen >= noaa::HRPT_FRAME_BITS) {
                    processFrame();
                }
                manchFramePos = 0;
                framesReceived++;

                // Periodically re-verify sync
                if (framesReceived > 100) {
                    synced = false;
                    framesReceived = 0;
                    syncWindowLen = 0;
                }
            }
        }
        else {
            // Searching for sync: fill correlation window
            if (syncWindowLen < noaa::HRPT_MANCH_SYNC_BITS) {
                syncWindow[syncWindowLen++] = bit;
            }
            else {
                // Shift window and add new bit
                memmove(syncWindow, syncWindow + 1, noaa::HRPT_MANCH_SYNC_BITS - 1);
                syncWindow[noaa::HRPT_MANCH_SYNC_BITS - 1] = bit;

                // Correlate against Manchester sync word
                int corr = noaa::correlateSync(syncWindow, manchSyncWord);
                if (corr >= noaa::HRPT_MANCH_SYNC_BITS - 12) {
                    // Sync found! Copy sync bits as start of frame
                    synced = true;
                    framesReceived = 0;
                    memcpy(manchFrame, syncWindow, noaa::HRPT_MANCH_SYNC_BITS);
                    manchFramePos = noaa::HRPT_MANCH_SYNC_BITS;
                    syncWindowLen = 0;
                }
            }
        }
    }

    void processFrame() {
        // Extract AVHRR channels from decoded frame
        uint16_t channels[noaa::HRPT_AVHRR_CHANNELS][noaa::HRPT_AVHRR_PIXELS];
        noaa::extractAVHRR(decodedFrame, channels);

        // Update greyscale images for each channel
        updateChannelImage(avhrr1Image, channels[0]);
        updateChannelImage(avhrr2Image, channels[1]);
        updateChannelImage(avhrr3Image, channels[2]);
        updateChannelImage(avhrr4Image, channels[3]);
        updateChannelImage(avhrr5Image, channels[4]);

        // RGB composite: channels 2,2,1 mapped to R,G,B (same as old code)
        uint8_t* buf = avhrrRGBImage.acquireNextLine();
        for (int i = 0; i < noaa::HRPT_AVHRR_PIXELS; i++) {
            float ch1 = ((float)channels[0][i] * 255.0f) / 1024.0f;
            float ch2 = ((float)channels[1][i] * 255.0f) / 1024.0f;
            buf[(i * 4)]     = (uint8_t)std::clamp(ch2, 0.0f, 255.0f); // R
            buf[(i * 4) + 1] = (uint8_t)std::clamp(ch2, 0.0f, 255.0f); // G
            buf[(i * 4) + 2] = (uint8_t)std::clamp(ch1, 0.0f, 255.0f); // B
            buf[(i * 4) + 3] = 255;                                      // A
        }
        avhrrRGBImage.releaseNextLine();
    }

    void updateChannelImage(ImGui::LinePushImage& img, const uint16_t* data) {
        uint8_t* buf = img.acquireNextLine();
        for (int i = 0; i < noaa::HRPT_AVHRR_PIXELS; i++) {
            float val = ((float)data[i] * 255.0f) / 1024.0f;
            uint8_t v = (uint8_t)std::clamp(val, 0.0f, 255.0f);
            buf[(i * 4)]     = v;
            buf[(i * 4) + 1] = v;
            buf[(i * 4) + 2] = v;
            buf[(i * 4) + 3] = 255;
        }
        img.releaseNextLine();
    }

    std::string _name;
    VFOManager::VFO* _vfo;

    // Sync word (Manchester encoded, 120 bits)
    uint8_t manchSyncWord[noaa::HRPT_MANCH_SYNC_BITS];

    // Sync detection window
    uint8_t syncWindow[noaa::HRPT_MANCH_SYNC_BITS];
    int syncWindowLen = 0;

    // Frame accumulation
    uint8_t* manchFrame;    // Manchester-encoded frame bits (221800)
    uint8_t* decodedFrame;  // Decoded frame bits (110900)
    int manchFramePos = 0;
    bool synced = false;
    int framesReceived = 0;

    // DSP chain
    dsp::demod::HRPT demod;
    dsp::routing::Splitter<dsp::complex_t> split;
    dsp::stream<dsp::complex_t> visStream;
    dsp::stream<dsp::complex_t> dataStream;
    dsp::buffer::Reshaper<dsp::complex_t> reshape;
    dsp::sink::Handler<dsp::complex_t> visSink;
    dsp::sink::Handler<dsp::complex_t> dataSink;

    // GUI
    ImGui::ConstellationDiagram constDiag;
    ImGui::LinePushImage avhrrRGBImage;
    ImGui::LinePushImage avhrr1Image;
    ImGui::LinePushImage avhrr2Image;
    ImGui::LinePushImage avhrr3Image;
    ImGui::LinePushImage avhrr4Image;
    ImGui::LinePushImage avhrr5Image;
    bool showWindow = false;
};
