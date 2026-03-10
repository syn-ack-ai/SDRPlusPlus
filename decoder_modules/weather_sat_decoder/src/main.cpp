#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <module.h>

#include <gui/widgets/constellation_diagram.h>

#include <sat_decoder.h>
#include <noaa_hrpt_decoder.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "weather_sat_decoder",
    /* Description:     */ "Weather Satellite Decoder for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 2, 0,
    /* Max instances    */ -1
};

class WeatherSatDecoderModule : public ModuleManager::Instance {
public:
    WeatherSatDecoderModule(std::string name) {
        this->name = name;

        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, 1000000, 1000000, 1000000, 1000000, true);

        decoders["NOAA HRPT"] = new NOAAHRPTDecoder(vfo, name);

        // Generate the decoder list for the combo box
        decoderNames.clear();
        decoderNamesStr = "";
        for (auto const& [name, dec] : decoders) {
            decoderNames.push_back(name);
            decoderNamesStr += name;
            decoderNamesStr += '\0';
        }

        selectDecoder(decoderNames[0], false);

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~WeatherSatDecoderModule() {
        decoder->stop();
        gui::menu.removeEntry(name);
        for (auto& [n, dec] : decoders) { delete dec; }
        sigpath::vfoManager.deleteVFO(vfo);
    }

    void postInit() {}

    void enable() {
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, 1000000, 1000000, 1000000, 1000000, true);
        for (auto const& [name, dec] : decoders) { dec->setVFO(vfo); }
        decoder->select();
        decoder->start();
        enabled = true;
    }

    void disable() {
        decoder->stop();
        sigpath::vfoManager.deleteVFO(vfo);
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    void selectDecoder(std::string name, bool deselectLast = true) {
        if (deselectLast) {
            decoder->stop();
        }
        decoder = decoders[name];
        decoder->select();
        decoder->start();
    }

    static void menuHandler(void* ctx) {
        WeatherSatDecoderModule* _this = (WeatherSatDecoderModule*)ctx;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        ImGui::SetNextItemWidth(menuWidth);
        if (ImGui::Combo(CONCAT("##weather_sat_dec_", _this->name), &_this->decoderId, _this->decoderNamesStr.c_str())) {
            _this->selectDecoder(_this->decoderNames[_this->decoderId]);
        }

        _this->decoder->drawMenu(menuWidth);

        if (!_this->enabled) { style::endDisabled(); }
    }

    std::string name;
    bool enabled = true;

    VFOManager::VFO* vfo;

    std::map<std::string, SatDecoder*> decoders;
    std::vector<std::string> decoderNames;
    std::string decoderNamesStr = "";
    int decoderId = 0;

    SatDecoder* decoder;
};

MOD_EXPORT void _INIT_() {
    // Nothing
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new WeatherSatDecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (WeatherSatDecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    // Nothing
}
