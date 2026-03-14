#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/style.h>
#include <config.h>
#include <sdrplay_api.h>
#include <gui/smgui.h>
#include <utils/optionlist.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "sdrplay_source",
    /* Description:     */ "SDRplay source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 2, 0,
    /* Max instances    */ 1
};

ConfigManager config;

sdrplay_api_Bw_MHzT preferedBandwidth[] = {
    sdrplay_api_BW_5_000,
    sdrplay_api_BW_5_000,
    sdrplay_api_BW_5_000,
    sdrplay_api_BW_5_000,
    sdrplay_api_BW_6_000,
    sdrplay_api_BW_7_000,
    sdrplay_api_BW_8_000,
    sdrplay_api_BW_8_000,
    sdrplay_api_BW_8_000
};

const sdrplay_api_Rsp2_AntennaSelectT rsp2_antennaPorts[] = {
    sdrplay_api_Rsp2_ANTENNA_A,
    sdrplay_api_Rsp2_ANTENNA_B,
    sdrplay_api_Rsp2_ANTENNA_B,
};

const char* rsp2_antennaPortsTxt = "Port A\0Port B\0Hi-Z\0";

const sdrplay_api_RspDx_AntennaSelectT rspdx_antennaPorts[] = {
    sdrplay_api_RspDx_ANTENNA_A,
    sdrplay_api_RspDx_ANTENNA_B,
    sdrplay_api_RspDx_ANTENNA_C
};

const char* rspdx_antennaPortsTxt = "Port A\0Port B\0Port C\0";

struct ifMode_t {
    sdrplay_api_If_kHzT ifValue;
    sdrplay_api_Bw_MHzT bw;
    unsigned int deviceSamplerate;
    unsigned int effectiveSamplerate;
};

ifMode_t ifModes[] = {
    { sdrplay_api_IF_Zero, sdrplay_api_BW_1_536, 2000000, 2000000 },
    { sdrplay_api_IF_2_048, sdrplay_api_BW_1_536, 8000000, 2000000 },
    { sdrplay_api_IF_2_048, sdrplay_api_BW_5_000, 8000000, 2000000 },
    { sdrplay_api_IF_1_620, sdrplay_api_BW_1_536, 6000000, 2000000 },
    { sdrplay_api_IF_0_450, sdrplay_api_BW_0_600, 2000000, 1000000 },
    { sdrplay_api_IF_0_450, sdrplay_api_BW_0_300, 2000000, 500000 },
    { sdrplay_api_IF_0_450, sdrplay_api_BW_0_200, 2000000, 500000 },
};

const char* ifModeTxt =
    "ZeroIF\0"
    "LowIF 2048KHz, IFBW 1536KHz\0"
    "LowIF 2048KHz, IFBW 5000KHz\0"
    "LowIF 1620KHz, IFBW 1536KHz\0"
    "LowIF 450KHz, IFBW 600KHz\0"
    "LowIF 450KHz, IFBW 300KHz\0"
    "LowIF 450KHz, IFBW 200KHz\0";

const char* rspduo_antennaPortsTxt = "Tuner 1 (50Ohm)\0Tuner 1 (Hi-Z)\0Tuner 2 (50Ohm)\0";

const char* usbTransferModeTxt = "Isochronous\0Bulk\0";
const char* decimationFactorTxt = "1 (Off)\0x2\0x4\0x8\0x16\0x32\0";
const unsigned char decimationFactors[] = { 1, 2, 4, 8, 16, 32 };

#define MAX_DEV_COUNT   16

class SDRPlaySourceModule : public ModuleManager::Instance {
public:
    SDRPlaySourceModule(std::string name) {
        this->name = name;

        // Init callbacks
        cbFuncs.EventCbFn = eventCB;
        cbFuncs.StreamACbFn = streamCB;
        cbFuncs.StreamBCbFn = streamCB;

        sdrplay_api_ErrT err = sdrplay_api_Open();
        if (err != sdrplay_api_Success) {
            flog::error("Could not intiatialized the SDRplay API. Make sure that the service is running.");
            return;
        }

        // Check API version compatibility
        float apiVer = 0.0f;
        err = sdrplay_api_ApiVersion(&apiVer);
        if (err == sdrplay_api_Success) {
            flog::info("SDRplay API version: {0}", apiVer);
            if (apiVer < 3.0f) {
                flog::error("SDRplay API version {0} is too old. Version 3.x or later is required.", apiVer);
                sdrplay_api_Close();
                return;
            }
        }
        else {
            flog::warn("Could not check SDRplay API version: {0}", sdrplay_api_GetErrorString(err));
        }

        sampleRate = 2000000.0;
        srId = 0;

        bandwidth = sdrplay_api_BW_5_000;
        bandwidthId = 8;

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;

        refresh();

        config.acquire();
        std::string confSelectDev = config.conf["device"];
        config.release();
        selectByName(confSelectDev);

        sigpath::sourceManager.registerSource("SDRplay", &handler);

        initOk = true;
    }

    ~SDRPlaySourceModule() {
        stop(this);
        if (initOk) { sdrplay_api_Close(); }
        sigpath::sourceManager.unregisterSource("SDRplay");
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

    void refresh() {
        devList.clear();
        devNameList.clear();
        devListTxt = "";

        sdrplay_api_DeviceT devArr[MAX_DEV_COUNT];
        unsigned int numDev = 0;

        // Lock the device API before enumerating devices (required by API spec)
        sdrplay_api_ErrT err = sdrplay_api_LockDeviceApi();
        if (err != sdrplay_api_Success) {
            flog::error("Could not lock SDRplay device API: {0}", sdrplay_api_GetErrorString(err));
            return;
        }

        err = sdrplay_api_GetDevices(devArr, &numDev, MAX_DEV_COUNT);
        if (err != sdrplay_api_Success) {
            flog::error("Could not get SDRplay devices: {0}", sdrplay_api_GetErrorString(err));
            sdrplay_api_UnlockDeviceApi();
            return;
        }

        sdrplay_api_UnlockDeviceApi();

        for (unsigned int i = 0; i < numDev; i++) {
            devList.push_back(devArr[i]);
            std::string name = "";
            switch (devArr[i].hwVer) {
            case SDRPLAY_RSP1_ID:
                name = "RSP1 (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            case SDRPLAY_RSP1A_ID:
                name = "RSP1A (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            case SDRPLAY_RSP1B_ID:
                name = "RSP1B (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            case SDRPLAY_RSP2_ID:
                name = "RSP2 (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            case SDRPLAY_RSPduo_ID:
                name = "RSPduo (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            case SDRPLAY_RSPdx_ID:
                name = "RSPdx (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            case SDRPLAY_RSPdxR2_ID:
                name = "RSPdx-R2 (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            default:
                name = "Unknown (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            }
            devNameList.push_back(name);
            devListTxt += name;
            devListTxt += '\0';
        }
    }

    void selectFirst() {
        if (devList.size() == 0) {
            selectedName = "";
            return;
        }
        selectDev(devList[0], 0);
    }

    void selectByName(std::string name) {
        for (size_t i = 0; i < devNameList.size(); i++) {
            if (devNameList[i] == name) {
                selectDev(devList[i], i);
                return;
            }
        }
        selectFirst();
    }

    void selectById(int id) {
        selectDev(devList[id], id);
    }

    void selectDev(sdrplay_api_DeviceT dev, int id) {
        openDev = dev;
        sdrplay_api_ErrT err;

        openDev.tuner = sdrplay_api_Tuner_A;
        openDev.rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
        err = sdrplay_api_SelectDevice(&openDev);
        if (err != sdrplay_api_Success) {
            const char* errStr = sdrplay_api_GetErrorString(err);
            flog::error("Could not select RSP device: {0}", errStr);
            selectedName = "";
            return;
        }

        sdrplay_api_UnlockDeviceApi();
        sdrplay_api_DebugEnable(openDev.dev, sdrplay_api_DbgLvl_Message);

        err = sdrplay_api_GetDeviceParams(openDev.dev, &openDevParams);
        if (err != sdrplay_api_Success) {
            const char* errStr = sdrplay_api_GetErrorString(err);
            flog::error("Could not get device params for RSP device: {0}", errStr);
            sdrplay_api_ReleaseDevice(&openDev);
            selectedName = "";
            return;
        }

        err = sdrplay_api_Init(openDev.dev, &cbFuncs, this);
        if (err != sdrplay_api_Success) {
            const char* errStr = sdrplay_api_GetErrorString(err);
            flog::error("Could not init RSP device: {0}", errStr);
            sdrplay_api_ReleaseDevice(&openDev);
            selectedName = "";
            return;
        }

        // Define the valid samplerates
        samplerates.clear();
        samplerates.define(2e6, "2MHz", 2e6);
        samplerates.define(3e6, "3MHz", 3e6);
        samplerates.define(4e6, "4MHz", 4e6);
        samplerates.define(5e6, "5MHz", 5e6);
        samplerates.define(6e6, "6MHz", 6e6);
        samplerates.define(7e6, "7MHz", 7e6);
        samplerates.define(8e6, "8MHz", 8e6);
        samplerates.define(9e6, "9MHz", 9e6);
        samplerates.define(10e6, "10MHz", 10e6);

        // Define the valid bandwidths
        bandwidths.clear();
        bandwidths.define(200e3, "200KHz", sdrplay_api_BW_0_200);
        bandwidths.define(300e3, "300KHz", sdrplay_api_BW_0_300);
        bandwidths.define(600e3, "600KHz", sdrplay_api_BW_0_600);
        bandwidths.define(1.536e6, "1.536MHz", sdrplay_api_BW_1_536);
        bandwidths.define(5e6, "5MHz", sdrplay_api_BW_5_000);
        bandwidths.define(6e6, "6MHz", sdrplay_api_BW_6_000);
        bandwidths.define(7e6, "7MHz", sdrplay_api_BW_7_000);
        bandwidths.define(8e6, "8MHz", sdrplay_api_BW_8_000);
        bandwidths.define(0, "Auto", sdrplay_api_BW_Undefined);

        channelParams = openDevParams->rxChannelA;

        selectedName = devNameList[id];

        if (openDev.hwVer == SDRPLAY_RSP1_ID) {
            lnaSteps = 4;
        }
        else if (openDev.hwVer == SDRPLAY_RSP1A_ID || openDev.hwVer == SDRPLAY_RSP1B_ID) {
            lnaSteps = 10;
        }
        else if (openDev.hwVer == SDRPLAY_RSP2_ID) {
            lnaSteps = 9;
        }
        else if (openDev.hwVer == SDRPLAY_RSPduo_ID) {
            lnaSteps = 10;
        }
        else if (openDev.hwVer == SDRPLAY_RSPdx_ID || openDev.hwVer == SDRPLAY_RSPdxR2_ID) {
            lnaSteps = 28;
        }

        // Select default settings
        srId = 0;
        sampleRate = samplerates.value(0);
        bandwidthId = 8;
        lnaGain = lnaSteps - 1;
        gain = 59;
        agc = false;
        agcAttack = 500;
        agcDecay = 500;
        agcDecayDelay = 200;
        agcDecayThreshold = 5;
        agcSetPoint = -30;
        ifModeId = 0;
        ppm = 0.0f;
        extendedGainRange = false;
        usbTransferMode = 0;
        decimationFactorId = 0;
        decimationWideband = false;
        dcCalMode = 3;
        dcCalSpeedUp = 0;
        dcCalTrackTime = 1;
        dcCalRefreshRate = 2048;
        rsp1a_fmmwNotch = false;
        rsp2_fmmwNotch = false;
        rspdx_fmmwNotch = false;
        rspduo_fmmwNotch = false;
        rsp1a_dabNotch = false;
        rspdx_dabNotch = false;
        rspduo_dabNotch = false;
        rsp1a_biasT = false;
        rsp2_biasT = false;
        rspdx_biasT = false;
        rspduo_biasT = false;
        rsp2_antennaPort = 0;
        rspdx_antennaPort = 0;
        rspduo_antennaPort = 0;

        config.acquire();

        // General options
        if (config.conf["devices"][selectedName].contains("samplerate")) {
            int sr = config.conf["devices"][selectedName]["samplerate"];
            if (samplerates.keyExists(sr)) {
                srId = samplerates.keyId(sr);
                sampleRate = samplerates[srId];
            }
        }
        if (config.conf["devices"][selectedName].contains("ifModeId")) {
            ifModeId = config.conf["devices"][selectedName]["ifModeId"];
            if (ifModeId != 0) {
                sampleRate = ifModes[ifModeId].effectiveSamplerate;
            }
        }
        if (config.conf["devices"][selectedName].contains("bwMode")) {
            bandwidthId = config.conf["devices"][selectedName]["bwMode"];
        }
        if (config.conf["devices"][selectedName].contains("lnaGain")) {
            lnaGain = config.conf["devices"][selectedName]["lnaGain"];
        }
        if (config.conf["devices"][selectedName].contains("ifGain")) {
            gain = config.conf["devices"][selectedName]["ifGain"];
        }
        if (config.conf["devices"][selectedName].contains("agc")) {
            agc = config.conf["devices"][selectedName]["agc"];
        }
        if (config.conf["devices"][selectedName].contains("agcAttack")) {
            agcAttack = config.conf["devices"][selectedName]["agcAttack"];
        }
        if (config.conf["devices"][selectedName].contains("agcDecay")) {
            agcDecay = config.conf["devices"][selectedName]["agcDecay"];
        }
        if (config.conf["devices"][selectedName].contains("agcDecayDelay")) {
            agcDecayDelay = config.conf["devices"][selectedName]["agcDecayDelay"];
        }
        if (config.conf["devices"][selectedName].contains("agcDecayThreshold")) {
            agcDecayThreshold = config.conf["devices"][selectedName]["agcDecayThreshold"];
        }
        if (config.conf["devices"][selectedName].contains("agcSetPoint")) {
            agcSetPoint = config.conf["devices"][selectedName]["agcSetPoint"];
        }
        if (config.conf["devices"][selectedName].contains("ppm")) {
            ppm = config.conf["devices"][selectedName]["ppm"];
        }
        if (config.conf["devices"][selectedName].contains("extendedGainRange")) {
            extendedGainRange = config.conf["devices"][selectedName]["extendedGainRange"];
        }
        if (config.conf["devices"][selectedName].contains("usbTransferMode")) {
            usbTransferMode = config.conf["devices"][selectedName]["usbTransferMode"];
        }
        if (config.conf["devices"][selectedName].contains("decimationFactor")) {
            decimationFactorId = config.conf["devices"][selectedName]["decimationFactor"];
        }
        if (config.conf["devices"][selectedName].contains("decimationWideband")) {
            decimationWideband = config.conf["devices"][selectedName]["decimationWideband"];
        }
        if (config.conf["devices"][selectedName].contains("dcCalMode")) {
            dcCalMode = config.conf["devices"][selectedName]["dcCalMode"];
        }
        if (config.conf["devices"][selectedName].contains("dcCalTrackTime")) {
            dcCalTrackTime = config.conf["devices"][selectedName]["dcCalTrackTime"];
        }
        if (config.conf["devices"][selectedName].contains("dcCalRefreshRate")) {
            dcCalRefreshRate = config.conf["devices"][selectedName]["dcCalRefreshRate"];
        }

        // Per device options
        if (openDev.hwVer == SDRPLAY_RSP1_ID) {
            // No config to load
        }
        else if (openDev.hwVer == SDRPLAY_RSP1A_ID || openDev.hwVer == SDRPLAY_RSP1B_ID) {
            if (config.conf["devices"][selectedName].contains("fmmwNotch")) {
                rsp1a_fmmwNotch = config.conf["devices"][selectedName]["fmmwNotch"];
            }
            if (config.conf["devices"][selectedName].contains("dabNotch")) {
                rsp1a_dabNotch = config.conf["devices"][selectedName]["dabNotch"];
            }
            if (config.conf["devices"][selectedName].contains("biast")) {
                rsp1a_biasT = config.conf["devices"][selectedName]["biast"];
            }
        }
        else if (openDev.hwVer == SDRPLAY_RSP2_ID) {
            if (config.conf["devices"][selectedName].contains("antenna")) {
                rsp2_antennaPort = config.conf["devices"][selectedName]["antenna"];
            }
            if (config.conf["devices"][selectedName].contains("fmmwNotch")) {
                rsp2_fmmwNotch = config.conf["devices"][selectedName]["fmmwNotch"];
            }
            if (config.conf["devices"][selectedName].contains("biast")) {
                rsp2_biasT = config.conf["devices"][selectedName]["biast"];
            }
        }
        else if (openDev.hwVer == SDRPLAY_RSPduo_ID) {
            if (config.conf["devices"][selectedName].contains("antenna")) {
                rspduo_antennaPort = config.conf["devices"][selectedName]["antenna"];
            }
            if (config.conf["devices"][selectedName].contains("fmmwNotch")) {
                rspduo_fmmwNotch = config.conf["devices"][selectedName]["fmmwNotch"];
            }
            if (config.conf["devices"][selectedName].contains("dabNotch")) {
                rspduo_dabNotch = config.conf["devices"][selectedName]["dabNotch"];
            }
            if (config.conf["devices"][selectedName].contains("biast")) {
                rspduo_biasT = config.conf["devices"][selectedName]["biast"];
            }
        }
        else if (openDev.hwVer == SDRPLAY_RSPdx_ID || openDev.hwVer == SDRPLAY_RSPdxR2_ID) {
            if (config.conf["devices"][selectedName].contains("antenna")) {
                rspdx_antennaPort = config.conf["devices"][selectedName]["antenna"];
            }
            if (config.conf["devices"][selectedName].contains("fmmwNotch")) {
                rspdx_fmmwNotch = config.conf["devices"][selectedName]["fmmwNotch"];
            }
            if (config.conf["devices"][selectedName].contains("dabNotch")) {
                rspdx_dabNotch = config.conf["devices"][selectedName]["dabNotch"];
            }
            if (config.conf["devices"][selectedName].contains("biast")) {
                rspdx_biasT = config.conf["devices"][selectedName]["biast"];
            }
        }

        config.release();

        if (lnaGain >= lnaSteps) { lnaGain = lnaSteps - 1; }

        // Release device after selecting
        sdrplay_api_Uninit(openDev.dev);
        sdrplay_api_ReleaseDevice(&openDev);
    }

    void rspDuoSelectTuner(sdrplay_api_TunerSelectT tuner, sdrplay_api_RspDuo_AmPortSelectT amPort) {
        if (openDev.tuner != tuner) {
            flog::info("Swapping tuners");
            auto ret = sdrplay_api_SwapRspDuoActiveTuner(openDev.dev, &openDev.tuner, amPort);
            if (ret != sdrplay_api_Success) {
                flog::error("Error while swapping tuners: {0}", sdrplay_api_GetErrorString(ret));
            }
        }

        // Change the channel params
        channelParams = (tuner == sdrplay_api_Tuner_A) ? openDevParams->rxChannelA : openDevParams->rxChannelB;
        channelParams->rspDuoTunerParams.tuner1AmPortSel = amPort;
        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_RspDuo_AmPortSelect, sdrplay_api_Update_Ext1_None);

        // Refresh gains (for some reason they're lost)
        channelParams->tunerParams.gain.LNAstate = lnaGain;
        channelParams->tunerParams.gain.gRdB = gain;
        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
    }

    void rspDuoSelectAntennaPort(int port) {
        if (port == 0) { rspDuoSelectTuner(sdrplay_api_Tuner_A, sdrplay_api_RspDuo_AMPORT_2); }
        if (port == 1) { rspDuoSelectTuner(sdrplay_api_Tuner_A, sdrplay_api_RspDuo_AMPORT_1); }
        if (port == 2) { rspDuoSelectTuner(sdrplay_api_Tuner_B, sdrplay_api_RspDuo_AMPORT_1); }
    }

private:
    static void menuSelected(void* ctx) {
        SDRPlaySourceModule* _this = (SDRPlaySourceModule*)ctx;
        core::setInputSampleRate(_this->sampleRate);
        flog::info("SDRPlaySourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        SDRPlaySourceModule* _this = (SDRPlaySourceModule*)ctx;
        flog::info("SDRPlaySourceModule '{0}': Menu Deselect!", _this->name);
    }

    static void start(void* ctx) {
        SDRPlaySourceModule* _this = (SDRPlaySourceModule*)ctx;
        if (_this->running) { return; }

        // First, acquire device
        sdrplay_api_ErrT err;

        _this->openDev.tuner = sdrplay_api_Tuner_A;
        _this->openDev.rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
        err = sdrplay_api_SelectDevice(&_this->openDev);
        if (err != sdrplay_api_Success) {
            const char* errStr = sdrplay_api_GetErrorString(err);
            flog::error("Could not select RSP device: {0}", errStr);
            return;
        }

        sdrplay_api_UnlockDeviceApi();
        sdrplay_api_DebugEnable(_this->openDev.dev, sdrplay_api_DbgLvl_Message);

        err = sdrplay_api_GetDeviceParams(_this->openDev.dev, &_this->openDevParams);
        if (err != sdrplay_api_Success) {
            const char* errStr = sdrplay_api_GetErrorString(err);
            flog::error("Could not get device params for RSP device: {0}", errStr);
            sdrplay_api_ReleaseDevice(&_this->openDev);
            return;
        }

        _this->channelParams = _this->openDevParams->rxChannelA;

        // Configure device parameters BEFORE sdrplay_api_Init
        _this->bufferIndex = 0;
        _this->bufferSize = (int)(_this->sampleRate / 200.0);

        // RSP1A Options
        if (_this->openDev.hwVer == SDRPLAY_RSP1A_ID || _this->openDev.hwVer == SDRPLAY_RSP1B_ID) {
            _this->openDevParams->devParams->rsp1aParams.rfNotchEnable = _this->rsp1a_fmmwNotch;
            _this->openDevParams->devParams->rsp1aParams.rfDabNotchEnable = _this->rsp1a_dabNotch;
            _this->channelParams->rsp1aTunerParams.biasTEnable = _this->rsp1a_biasT;
        }
        else if (_this->openDev.hwVer == SDRPLAY_RSP2_ID) {
            _this->channelParams->rsp2TunerParams.rfNotchEnable = _this->rsp2_fmmwNotch;
            _this->channelParams->rsp2TunerParams.biasTEnable = _this->rsp2_biasT;
            _this->channelParams->rsp2TunerParams.antennaSel = rsp2_antennaPorts[_this->rsp2_antennaPort];
            _this->channelParams->rsp2TunerParams.amPortSel = (_this->rsp2_antennaPort == 2) ? sdrplay_api_Rsp2_AMPORT_1 : sdrplay_api_Rsp2_AMPORT_2;
        }
        else if (_this->openDev.hwVer == SDRPLAY_RSPduo_ID) {
            _this->channelParams->rspDuoTunerParams.biasTEnable = _this->rspduo_biasT;
            _this->channelParams->rspDuoTunerParams.rfNotchEnable = _this->rspduo_fmmwNotch;
            _this->channelParams->rspDuoTunerParams.rfDabNotchEnable = _this->rspduo_dabNotch;
            _this->channelParams->rspDuoTunerParams.tuner1AmNotchEnable = _this->rspduo_fmmwNotch;
        }
        else if (_this->openDev.hwVer == SDRPLAY_RSPdx_ID || _this->openDev.hwVer == SDRPLAY_RSPdxR2_ID) {
            _this->openDevParams->devParams->rspDxParams.rfNotchEnable = _this->rspdx_fmmwNotch;
            _this->openDevParams->devParams->rspDxParams.rfDabNotchEnable = _this->rspdx_dabNotch;
            _this->openDevParams->devParams->rspDxParams.biasTEnable = _this->rspdx_biasT;
            _this->openDevParams->devParams->rspDxParams.antennaSel = rspdx_antennaPorts[_this->rspdx_antennaPort];
        }

        // General options - set before Init so device starts with correct params
        if (_this->ifModeId == 0) {
            _this->bandwidth = (_this->bandwidthId == 8) ? preferedBandwidth[_this->srId] : _this->bandwidths[_this->bandwidthId];
            _this->openDevParams->devParams->fsFreq.fsHz = _this->sampleRate;
            _this->channelParams->tunerParams.bwType = _this->bandwidth;
        }
        else {
            _this->openDevParams->devParams->fsFreq.fsHz = ifModes[_this->ifModeId].deviceSamplerate;
            _this->channelParams->tunerParams.bwType = ifModes[_this->ifModeId].bw;
        }
        _this->channelParams->tunerParams.rfFreq.rfHz = _this->freq;
        _this->channelParams->tunerParams.gain.gRdB = _this->gain;
        _this->channelParams->tunerParams.gain.LNAstate = _this->lnaGain;
        _this->channelParams->tunerParams.ifType = ifModes[_this->ifModeId].ifValue;
        _this->channelParams->tunerParams.loMode = sdrplay_api_LO_Auto;

        // Extended gain range
        _this->channelParams->tunerParams.gain.minGr = _this->extendedGainRange ? sdrplay_api_EXTENDED_MIN_GR : sdrplay_api_NORMAL_MIN_GR;

        // PPM correction
        _this->openDevParams->devParams->ppm = _this->ppm;

        // USB transfer mode
        _this->openDevParams->devParams->mode = _this->usbTransferMode == 0 ? sdrplay_api_ISOCH : sdrplay_api_BULK;

        // DC offset and IQ correction
        _this->channelParams->ctrlParams.dcOffset.DCenable = true;
        _this->channelParams->ctrlParams.dcOffset.IQenable = true;

        // DC offset tuner calibration
        _this->channelParams->tunerParams.dcOffsetTuner.dcCal = _this->dcCalMode;
        _this->channelParams->tunerParams.dcOffsetTuner.speedUp = _this->dcCalSpeedUp;
        _this->channelParams->tunerParams.dcOffsetTuner.trackTime = _this->dcCalTrackTime;
        _this->channelParams->tunerParams.dcOffsetTuner.refreshRateTime = _this->dcCalRefreshRate;

        // Decimation
        if (_this->decimationFactorId > 0) {
            _this->channelParams->ctrlParams.decimation.enable = true;
            _this->channelParams->ctrlParams.decimation.decimationFactor = decimationFactors[_this->decimationFactorId];
            _this->channelParams->ctrlParams.decimation.wideBandSignal = _this->decimationWideband ? 1 : 0;
        }
        else {
            _this->channelParams->ctrlParams.decimation.enable = false;
            _this->channelParams->ctrlParams.decimation.decimationFactor = 1;
            _this->channelParams->ctrlParams.decimation.wideBandSignal = 0;
        }

        // AGC parameters
        _this->channelParams->ctrlParams.agc.attack_ms = _this->agcAttack;
        _this->channelParams->ctrlParams.agc.decay_ms = _this->agcDecay;
        _this->channelParams->ctrlParams.agc.decay_delay_ms = _this->agcDecayDelay;
        _this->channelParams->ctrlParams.agc.decay_threshold_dB = _this->agcDecayThreshold;
        _this->channelParams->ctrlParams.agc.setPoint_dBfs = _this->agcSetPoint;
        _this->channelParams->ctrlParams.agc.enable = _this->agc ? sdrplay_api_AGC_50HZ : sdrplay_api_AGC_DISABLE;

        // Now init the device - it will use all the parameters set above
        err = sdrplay_api_Init(_this->openDev.dev, &_this->cbFuncs, _this);
        if (err != sdrplay_api_Success) {
            const char* errStr = sdrplay_api_GetErrorString(err);
            flog::error("Could not init RSP device: {0}", errStr);
            sdrplay_api_ReleaseDevice(&_this->openDev);
            return;
        }

        // Apply device-specific options that require Update after Init
        if (_this->openDev.hwVer == SDRPLAY_RSP1A_ID || _this->openDev.hwVer == SDRPLAY_RSP1B_ID) {
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Rsp1a_RfNotchControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Rsp1a_RfDabNotchControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Rsp1a_BiasTControl, sdrplay_api_Update_Ext1_None);
        }
        else if (_this->openDev.hwVer == SDRPLAY_RSP2_ID) {
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Rsp2_RfNotchControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Rsp2_BiasTControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Rsp2_AntennaControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Rsp2_AmPortSelect, sdrplay_api_Update_Ext1_None);
        }
        else if (_this->openDev.hwVer == SDRPLAY_RSPduo_ID) {
            _this->rspDuoSelectAntennaPort(_this->rspduo_antennaPort);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_RspDuo_BiasTControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_RspDuo_RfNotchControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_RspDuo_RfDabNotchControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_RspDuo_Tuner1AmNotchControl, sdrplay_api_Update_Ext1_None);
        }
        else if (_this->openDev.hwVer == SDRPLAY_RSPdx_ID || _this->openDev.hwVer == SDRPLAY_RSPdxR2_ID) {
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_RfNotchControl);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_RfDabNotchControl);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_BiasTControl);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_AntennaControl);
        }

        _this->running = true;
        flog::info("SDRPlaySourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        SDRPlaySourceModule* _this = (SDRPlaySourceModule*)ctx;
        if (!_this->running) { return; }
        _this->running = false;
        _this->stream.stopWriter();

        // Release device after stopping
        sdrplay_api_Uninit(_this->openDev.dev);
        sdrplay_api_ReleaseDevice(&_this->openDev);

        _this->stream.clearWriteStop();
        flog::info("SDRPlaySourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double freq, void* ctx) {
        SDRPlaySourceModule* _this = (SDRPlaySourceModule*)ctx;
        if (_this->running) {
            _this->channelParams->tunerParams.rfFreq.rfHz = freq;
            sdrplay_api_ErrT err = sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Tuner_Frf, sdrplay_api_Update_Ext1_None);
            if (err != sdrplay_api_Success) {
                flog::error("SDRPlaySourceModule '{0}': Tune update failed: {1}", _this->name, sdrplay_api_GetErrorString(err));
            }
        }
        _this->freq = freq;
        flog::info("SDRPlaySourceModule '{0}': Tune: {1}!", _this->name, freq);
    }

    static void menuHandler(void* ctx) {
        SDRPlaySourceModule* _this = (SDRPlaySourceModule*)ctx;

        if (_this->running) { SmGui::BeginDisabled(); }

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##sdrplay_dev", _this->name), &_this->devId, _this->devListTxt.c_str())) {
            _this->selectById(_this->devId);
            core::setInputSampleRate(_this->sampleRate);
            config.acquire();
            config.conf["device"] = _this->devNameList[_this->devId];
            config.release(true);
        }

        if (_this->ifModeId == 0) {
            if (SmGui::Combo(CONCAT("##sdrplay_sr", _this->name), &_this->srId, _this->samplerates.txt)) {
                _this->sampleRate = _this->samplerates[_this->srId];
                if (_this->bandwidthId == 8) {
                    _this->bandwidth = preferedBandwidth[_this->srId];
                }
                core::setInputSampleRate(_this->sampleRate);
                config.acquire();
                config.conf["devices"][_this->selectedName]["samplerate"] = _this->samplerates.key(_this->srId);
                config.release(true);
            }

            SmGui::SameLine();
            SmGui::FillWidth();
            SmGui::ForceSync();
            if (SmGui::Button(CONCAT("Refresh##sdrplay_refresh", _this->name))) {
                _this->refresh();
                _this->selectByName(_this->selectedName);
                core::setInputSampleRate(_this->sampleRate);
            }

            SmGui::LeftLabel("Bandwidth");
            SmGui::FillWidth();
            if (SmGui::Combo(CONCAT("##sdrplay_bw", _this->name), &_this->bandwidthId, _this->bandwidths.txt)) {
                _this->bandwidth = (_this->bandwidthId == 8) ? preferedBandwidth[_this->srId] : _this->bandwidths[_this->bandwidthId];
                if (_this->running) {
                    _this->channelParams->tunerParams.bwType = _this->bandwidth;
                    sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Tuner_BwType, sdrplay_api_Update_Ext1_None);
                }
                config.acquire();
                config.conf["devices"][_this->selectedName]["bwMode"] = _this->bandwidthId;
                config.release(true);
            }
        }
        else {
            SmGui::FillWidth();
            SmGui::ForceSync();
            if (SmGui::Button(CONCAT("Refresh##sdrplay_refresh", _this->name))) {
                _this->refresh();
                _this->selectByName(_this->selectedName);
            }
        }

        SmGui::LeftLabel("IF Mode");
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##sdrplay_ifmode", _this->name), &_this->ifModeId, ifModeTxt)) {
            if (_this->ifModeId != 0) {
                _this->bandwidth = ifModes[_this->ifModeId].bw;
                _this->sampleRate = ifModes[_this->ifModeId].effectiveSamplerate;
            }
            else {
                config.acquire();
                // Reload samplerate
                if (config.conf["devices"][_this->selectedName].contains("samplerate")) {
                    int sr = config.conf["devices"][_this->selectedName]["samplerate"];
                    if (_this->samplerates.keyExists(sr)) {
                        _this->srId = _this->samplerates.keyId(sr);
                    }
                }
                else {
                    _this->srId = 0;
                }

                // Reload bandwidth
                if (config.conf["devices"][_this->selectedName].contains("bwMode")) {
                    _this->bandwidthId = config.conf["devices"][_this->selectedName]["bwMode"];
                }
                else {
                    // Auto
                    _this->bandwidthId = 8;
                }
                _this->sampleRate = _this->samplerates[_this->srId];
                config.release();
                _this->bandwidth = (_this->bandwidthId == 8) ? preferedBandwidth[_this->srId] : _this->bandwidths[_this->bandwidthId];
            }
            core::setInputSampleRate(_this->sampleRate);
            config.acquire();
            config.conf["devices"][_this->selectedName]["ifModeId"] = _this->ifModeId;
            config.release(true);
        }

        if (_this->running) { SmGui::EndDisabled(); }

        if (_this->selectedName != "") {
            SmGui::LeftLabel("LNA Gain");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##sdrplay_lna_gain", _this->name), &_this->lnaGain, _this->lnaSteps - 1, 0, SmGui::FMT_STR_NONE)) {
                if (_this->running) {
                    _this->channelParams->tunerParams.gain.LNAstate = _this->lnaGain;
                    sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
                }
                config.acquire();
                config.conf["devices"][_this->selectedName]["lnaGain"] = _this->lnaGain;
                config.release(true);
            }

            if (_this->agc > 0) { SmGui::BeginDisabled(); }
            SmGui::LeftLabel("IF Gain");
            SmGui::FillWidth();
            int minGain = _this->extendedGainRange ? 0 : 20;
            if (SmGui::SliderInt(CONCAT("##sdrplay_gain", _this->name), &_this->gain, 59, minGain, SmGui::FMT_STR_NONE)) {
                if (_this->running) {
                    _this->channelParams->tunerParams.gain.gRdB = _this->gain;
                    sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
                }
                config.acquire();
                config.conf["devices"][_this->selectedName]["ifGain"] = _this->gain;
                config.release(true);
            }
            if (_this->agc > 0) { SmGui::EndDisabled(); }


            if (_this->agcParamEdit) {
                bool valid = false;
                _this->agcParamEdit = _this->agcParamMenu(valid);

                // If the menu was closed and (TODO) valid, update options
                if (!_this->agcParamEdit && valid) {
                    _this->agcAttack = _this->_agcAttack;
                    _this->agcDecay = _this->_agcDecay;
                    _this->agcDecayDelay = _this->_agcDecayDelay;
                    _this->agcDecayThreshold = _this->_agcDecayThreshold;
                    _this->agcSetPoint = _this->_agcSetPoint;
                    if (_this->running && _this->agc) {
                        _this->channelParams->ctrlParams.agc.attack_ms = _this->agcAttack;
                        _this->channelParams->ctrlParams.agc.decay_ms = _this->agcDecay;
                        _this->channelParams->ctrlParams.agc.decay_delay_ms = _this->agcDecayDelay;
                        _this->channelParams->ctrlParams.agc.decay_threshold_dB = _this->agcDecayThreshold;
                        _this->channelParams->ctrlParams.agc.setPoint_dBfs = _this->agcSetPoint;
                        sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
                    }
                    config.acquire();
                    config.conf["devices"][_this->selectedName]["agcAttack"] = _this->agcAttack;
                    config.conf["devices"][_this->selectedName]["agcDecay"] = _this->agcDecay;
                    config.conf["devices"][_this->selectedName]["agcDecayDelay"] = _this->agcDecayDelay;
                    config.conf["devices"][_this->selectedName]["agcDecayThreshold"] = _this->agcDecayThreshold;
                    config.conf["devices"][_this->selectedName]["agcSetPoint"] = _this->agcSetPoint;
                    config.release(true);
                }
            }

            SmGui::ForceSync();
            if (SmGui::Checkbox(CONCAT("IF AGC##sdrplay_agc", _this->name), &_this->agc)) {
                if (_this->running) {
                    _this->channelParams->ctrlParams.agc.enable = _this->agc ? sdrplay_api_AGC_50HZ : sdrplay_api_AGC_DISABLE;
                    if (_this->agc) {
                        _this->channelParams->ctrlParams.agc.attack_ms = _this->agcAttack;
                        _this->channelParams->ctrlParams.agc.decay_ms = _this->agcDecay;
                        _this->channelParams->ctrlParams.agc.decay_delay_ms = _this->agcDecayDelay;
                        _this->channelParams->ctrlParams.agc.decay_threshold_dB = _this->agcDecayThreshold;
                        _this->channelParams->ctrlParams.agc.setPoint_dBfs = _this->agcSetPoint;
                        sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
                    }
                    else {
                        sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
                        _this->channelParams->tunerParams.gain.gRdB = _this->gain;
                        sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
                    }
                }
                config.acquire();
                config.conf["devices"][_this->selectedName]["agc"] = _this->agc;
                config.release(true);
            }
            SmGui::SameLine();
            SmGui::FillWidth();
            SmGui::ForceSync();
            if (SmGui::Button(CONCAT("Parameters##sdrplay_agc_edit_btn", _this->name))) {
                _this->agcParamEdit = true;
                _this->_agcAttack = _this->agcAttack;
                _this->_agcDecay = _this->agcDecay;
                _this->_agcDecayDelay = _this->agcDecayDelay;
                _this->_agcDecayThreshold = _this->agcDecayThreshold;
                _this->_agcSetPoint = _this->agcSetPoint;
            }

            // PPM Correction
            SmGui::LeftLabel("PPM Correction");
            SmGui::FillWidth();
            if (SmGui::SliderFloatWithSteps(CONCAT("##sdrplay_ppm", _this->name), &_this->ppm, -100.0f, 100.0f, 0.1f)) {
                if (_this->running) {
                    _this->openDevParams->devParams->ppm = _this->ppm;
                    sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Dev_Ppm, sdrplay_api_Update_Ext1_None);
                }
                config.acquire();
                config.conf["devices"][_this->selectedName]["ppm"] = _this->ppm;
                config.release(true);
            }

            // Extended Gain Range
            if (SmGui::Checkbox(CONCAT("Extended Gain Range##sdrplay_extgr", _this->name), &_this->extendedGainRange)) {
                if (_this->running) {
                    _this->channelParams->tunerParams.gain.minGr = _this->extendedGainRange ? sdrplay_api_EXTENDED_MIN_GR : sdrplay_api_NORMAL_MIN_GR;
                    sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
                }
                config.acquire();
                config.conf["devices"][_this->selectedName]["extendedGainRange"] = _this->extendedGainRange;
                config.release(true);
            }

            // USB Transfer Mode
            if (_this->running) { SmGui::BeginDisabled(); }
            SmGui::LeftLabel("USB Mode");
            SmGui::FillWidth();
            if (SmGui::Combo(CONCAT("##sdrplay_usb_mode", _this->name), &_this->usbTransferMode, usbTransferModeTxt)) {
                config.acquire();
                config.conf["devices"][_this->selectedName]["usbTransferMode"] = _this->usbTransferMode;
                config.release(true);
            }

            // Decimation
            SmGui::LeftLabel("Decimation");
            SmGui::FillWidth();
            if (SmGui::Combo(CONCAT("##sdrplay_decim", _this->name), &_this->decimationFactorId, decimationFactorTxt)) {
                config.acquire();
                config.conf["devices"][_this->selectedName]["decimationFactor"] = _this->decimationFactorId;
                config.release(true);
            }
            if (_this->running) { SmGui::EndDisabled(); }

            if (_this->decimationFactorId > 0) {
                if (SmGui::Checkbox(CONCAT("Wideband Decimation##sdrplay_decim_wb", _this->name), &_this->decimationWideband)) {
                    if (_this->running) {
                        _this->channelParams->ctrlParams.decimation.wideBandSignal = _this->decimationWideband ? 1 : 0;
                        sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Ctrl_Decimation, sdrplay_api_Update_Ext1_None);
                    }
                    config.acquire();
                    config.conf["devices"][_this->selectedName]["decimationWideband"] = _this->decimationWideband;
                    config.release(true);
                }
            }

            switch (_this->openDev.hwVer) {
            case SDRPLAY_RSP1_ID:
                _this->RSP1Menu();
                break;
            case SDRPLAY_RSP1A_ID:
            case SDRPLAY_RSP1B_ID:
                _this->RSP1AMenu();
                break;
            case SDRPLAY_RSP2_ID:
                _this->RSP2Menu();
                break;
            case SDRPLAY_RSPduo_ID:
                _this->RSPduoMenu();
                break;
            case SDRPLAY_RSPdx_ID:
            case SDRPLAY_RSPdxR2_ID:
                _this->RSPdxMenu();
                break;
            default:
                _this->RSPUnsupportedMenu();
                break;
            }
        }
        else {
            SmGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "No device available");
        }
    }

    bool agcParamMenu(bool& valid) {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;
        SmGui::OpenPopup("Edit##sdrplay_source_edit_agc_params_");
        if (SmGui::BeginPopup("Edit##sdrplay_source_edit_agc_params_", ImGuiWindowFlags_NoResize)) {
            if (SmGui::BeginTable(("sdrplay_source_agc_param_tbl" + name).c_str(), 2)) {
                SmGui::TableNextRow();
                SmGui::TableSetColumnIndex(0);
                SmGui::LeftLabel("Attack");
                SmGui::TableSetColumnIndex(1);
                SmGui::SetNextItemWidth(100);
                SmGui::InputInt("ms##sdrplay_source_agc_attack", &_agcAttack);
                _agcAttack = std::clamp<int>(_agcAttack, 0, 65535);

                SmGui::TableNextRow();
                SmGui::TableSetColumnIndex(0);
                SmGui::LeftLabel("Decay");
                SmGui::TableSetColumnIndex(1);
                SmGui::SetNextItemWidth(100);
                SmGui::InputInt("ms##sdrplay_source_agc_decay", &_agcDecay);
                _agcDecay = std::clamp<int>(_agcDecay, 0, 65535);

                SmGui::TableNextRow();
                SmGui::TableSetColumnIndex(0);
                SmGui::LeftLabel("Decay Delay");
                SmGui::TableSetColumnIndex(1);
                SmGui::SetNextItemWidth(100);
                SmGui::InputInt("ms##sdrplay_source_agc_decay_delay", &_agcDecayDelay);
                _agcDecayDelay = std::clamp<int>(_agcDecayDelay, 0, 65535);

                SmGui::TableNextRow();
                SmGui::TableSetColumnIndex(0);
                SmGui::LeftLabel("Decay Threshold");
                SmGui::TableSetColumnIndex(1);
                SmGui::SetNextItemWidth(100);
                SmGui::InputInt("dB##sdrplay_source_agc_decay_thresh", &_agcDecayThreshold);
                _agcDecayThreshold = std::clamp<int>(_agcDecayThreshold, 0, 100);

                SmGui::TableNextRow();
                SmGui::TableSetColumnIndex(0);
                SmGui::LeftLabel("Setpoint");
                SmGui::TableSetColumnIndex(1);
                SmGui::SetNextItemWidth(100);
                SmGui::InputInt("dBFS##sdrplay_source_agc_setpoint", &_agcSetPoint);
                _agcSetPoint = std::clamp<int>(_agcSetPoint, -60, -20);

                SmGui::EndTable();
            }

            SmGui::ForceSync();
            if (SmGui::Button(" Apply ")) {
                open = false;
                valid = true;
            }
            SmGui::SameLine();
            SmGui::ForceSync();
            if (SmGui::Button("Cancel")) {
                open = false;
                valid = false;
            }
            SmGui::EndPopup();
        }
        return open;
    }

    void RSP1Menu() {
        // No options?
    }

    void RSP1AMenu() {
        if (SmGui::Checkbox(CONCAT("FM/MW Notch##sdrplay_rsp1a_fmmwnotch", name), &rsp1a_fmmwNotch)) {
            if (running) {
                openDevParams->devParams->rsp1aParams.rfNotchEnable = rsp1a_fmmwNotch;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp1a_RfNotchControl, sdrplay_api_Update_Ext1_None);
            }
            config.acquire();
            config.conf["devices"][selectedName]["fmmwNotch"] = rsp1a_fmmwNotch;
            config.release(true);
        }
        if (SmGui::Checkbox(CONCAT("DAB Notch##sdrplay_rsp1a_dabnotch", name), &rsp1a_dabNotch)) {
            if (running) {
                openDevParams->devParams->rsp1aParams.rfDabNotchEnable = rsp1a_dabNotch;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp1a_RfDabNotchControl, sdrplay_api_Update_Ext1_None);
            }
            config.acquire();
            config.conf["devices"][selectedName]["dabNotch"] = rsp1a_dabNotch;
            config.release(true);
        }
        if (SmGui::Checkbox(CONCAT("Bias-T##sdrplay_rsp1a_biast", name), &rsp1a_biasT)) {
            if (running) {
                channelParams->rsp1aTunerParams.biasTEnable = rsp1a_biasT;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp1a_BiasTControl, sdrplay_api_Update_Ext1_None);
            }
            config.acquire();
            config.conf["devices"][selectedName]["biast"] = rsp1a_biasT;
            config.release(true);
        }
    }

    void RSP2Menu() {
        SmGui::LeftLabel("Antenna");
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##sdrplay_rsp2_ant", name), &rsp2_antennaPort, rsp2_antennaPortsTxt)) {
            if (running) {
                channelParams->rsp2TunerParams.antennaSel = rsp2_antennaPorts[rsp2_antennaPort];
                channelParams->rsp2TunerParams.amPortSel = (rsp2_antennaPort == 2) ? sdrplay_api_Rsp2_AMPORT_1 : sdrplay_api_Rsp2_AMPORT_2;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp2_AntennaControl, sdrplay_api_Update_Ext1_None);
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp2_AmPortSelect, sdrplay_api_Update_Ext1_None);
            }
            config.acquire();
            config.conf["devices"][selectedName]["antenna"] = rsp2_antennaPort;
            config.release(true);
        }

        // The notch is only available on the 50Ohm ports
        if (rsp2_antennaPort != 2) {
            if (SmGui::Checkbox(CONCAT("MW/FM Notch##sdrplay_rsp2_fmmwnotch", name), &rsp2_fmmwNotch)) {
                if (running) {
                    channelParams->rsp2TunerParams.rfNotchEnable = rsp2_fmmwNotch;
                    sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp2_RfNotchControl, sdrplay_api_Update_Ext1_None);
                }
                config.acquire();
                config.conf["devices"][selectedName]["fmmwNotch"] = rsp2_fmmwNotch;
                config.release(true);
            }
        }
        else {
            style::beginDisabled();
            bool dummy = false;
            SmGui::Checkbox(CONCAT("MW/FM Notch##sdrplay_rsp2_fmmwnotch", name), &dummy);
            style::endDisabled();
        }
        
        if (SmGui::Checkbox(CONCAT("Bias-T##sdrplay_rsp2_biast", name), &rsp2_biasT)) {
            if (running) {
                channelParams->rsp2TunerParams.biasTEnable = rsp2_biasT;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp2_BiasTControl, sdrplay_api_Update_Ext1_None);
            }
            config.acquire();
            config.conf["devices"][selectedName]["biast"] = rsp2_biasT;
            config.release(true);
        }
    }

    void RSPduoMenu() {
        SmGui::LeftLabel("Antenna");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##sdrplay_rspduo_ant", name), &rspduo_antennaPort, rspduo_antennaPortsTxt)) {
            if (running) {
                rspDuoSelectAntennaPort(rspduo_antennaPort);
            }
            config.acquire();
            config.conf["devices"][selectedName]["antenna"] = rspduo_antennaPort;
            config.release(true);
        }
        if (SmGui::Checkbox(CONCAT("FM/MW Notch##sdrplay_rspduo_fmmwnotch", name), &rspduo_fmmwNotch)) {
            if (running) {
                channelParams->rspDuoTunerParams.rfNotchEnable = rspduo_fmmwNotch;
                channelParams->rspDuoTunerParams.tuner1AmNotchEnable = rspduo_fmmwNotch;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_RspDuo_RfNotchControl, sdrplay_api_Update_Ext1_None);
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_RspDuo_Tuner1AmNotchControl, sdrplay_api_Update_Ext1_None);
            }
            config.acquire();
            config.conf["devices"][selectedName]["fmmwnotch"] = rspduo_fmmwNotch;
            config.release(true);
        }
        if (SmGui::Checkbox(CONCAT("DAB Notch##sdrplay_rspduo_dabnotch", name), &rspduo_dabNotch)) {
            if (running) {
                channelParams->rspDuoTunerParams.rfDabNotchEnable = rspduo_dabNotch;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_RspDuo_RfDabNotchControl, sdrplay_api_Update_Ext1_None);
            }
            config.acquire();
            config.conf["devices"][selectedName]["dabNotch"] = rspduo_dabNotch;
            config.release(true);
        }
        if (SmGui::Checkbox(CONCAT("Bias-T##sdrplay_rspduo_biast", name), &rspduo_biasT)) {
            if (running) {
                channelParams->rspDuoTunerParams.biasTEnable = rspduo_biasT;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_RspDuo_BiasTControl, sdrplay_api_Update_Ext1_None);
            }
            config.acquire();
            config.conf["devices"][selectedName]["biast"] = rspduo_biasT;
            config.release(true);
        }
    }

    void RSPdxMenu() {
        SmGui::LeftLabel("Antenna");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##sdrplay_rspdx_ant", name), &rspdx_antennaPort, rspdx_antennaPortsTxt)) {
            if (running) {
                openDevParams->devParams->rspDxParams.antennaSel = rspdx_antennaPorts[rspdx_antennaPort];
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_AntennaControl);
            }
            config.acquire();
            config.conf["devices"][selectedName]["antenna"] = rspdx_antennaPort;
            config.release(true);
        }

        if (SmGui::Checkbox(CONCAT("FM/MW Notch##sdrplay_rspdx_fmmwnotch", name), &rspdx_fmmwNotch)) {
            if (running) {
                openDevParams->devParams->rspDxParams.rfNotchEnable = rspdx_fmmwNotch;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_RfNotchControl);
            }
            config.acquire();
            config.conf["devices"][selectedName]["fmmwNotch"] = rspdx_fmmwNotch;
            config.release(true);
        }
        if (SmGui::Checkbox(CONCAT("DAB Notch##sdrplay_rspdx_dabnotch", name), &rspdx_dabNotch)) {
            if (running) {
                openDevParams->devParams->rspDxParams.rfDabNotchEnable = rspdx_dabNotch;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_RfDabNotchControl);
            }
            config.acquire();
            config.conf["devices"][selectedName]["dabNotch"] = rspdx_dabNotch;
            config.release(true);
        }
        if (SmGui::Checkbox(CONCAT("Bias-T##sdrplay_rspdx_biast", name), &rspdx_biasT)) {
            if (running) {
                openDevParams->devParams->rspDxParams.biasTEnable = rspdx_biasT;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_BiasTControl);
            }
            config.acquire();
            config.conf["devices"][selectedName]["biast"] = rspdx_biasT;
            config.release(true);
        }
    }

    void RSPUnsupportedMenu() {
        SmGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Device currently unsupported");
    }

    static void streamCB(short* xi, short* xq, sdrplay_api_StreamCbParamsT* params,
                         unsigned int numSamples, unsigned int reset, void* cbContext) {
        SDRPlaySourceModule* _this = (SDRPlaySourceModule*)cbContext;
        if (!_this->running) { return; }

        // Handle stream reset (triggered by frequency change, gain change, etc.)
        if (reset) {
            flog::info("SDRPlaySourceModule '{0}': Stream reset requested", _this->name);
            _this->bufferIndex = 0;
        }

        // Log parameter changes signaled by the API
        if (params->grChanged) {
            flog::debug("SDRPlaySourceModule '{0}': Gain reduction changed (sample {1})", _this->name, params->firstSampleNum);
        }
        if (params->rfChanged) {
            flog::debug("SDRPlaySourceModule '{0}': RF frequency changed (sample {1})", _this->name, params->firstSampleNum);
        }
        if (params->fsChanged) {
            flog::debug("SDRPlaySourceModule '{0}': Sample rate changed (sample {1})", _this->name, params->firstSampleNum);
        }

        for (unsigned int i = 0; i < numSamples; i++) {
            int id = _this->bufferIndex++;
            _this->stream.writeBuf[id].re = (float)xi[i] / 32768.0f;
            _this->stream.writeBuf[id].im = (float)xq[i] / 32768.0f;

            if (_this->bufferIndex >= _this->bufferSize) {
                _this->stream.swap(_this->bufferSize);
                _this->bufferIndex = 0;
            }
        }
    }

    static void eventCB(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner,
                        sdrplay_api_EventParamsT* params, void* cbContext) {
        SDRPlaySourceModule* _this = (SDRPlaySourceModule*)cbContext;

        switch (eventId) {
        case sdrplay_api_PowerOverloadChange:
            // Must acknowledge power overload events per API spec
            sdrplay_api_Update(_this->openDev.dev, tuner, sdrplay_api_Update_Ctrl_OverloadMsgAck, sdrplay_api_Update_Ext1_None);
            if (params->powerOverloadParams.powerOverloadChangeType == sdrplay_api_Overload_Detected) {
                flog::warn("SDRPlaySourceModule '{0}': ADC power overload detected", _this->name);
            }
            else {
                flog::info("SDRPlaySourceModule '{0}': ADC power overload corrected", _this->name);
            }
            break;
        case sdrplay_api_GainChange:
            flog::debug("SDRPlaySourceModule '{0}': Gain change - gRdB={1}, lnaGRdB={2}", _this->name,
                        params->gainParams.gRdB, params->gainParams.lnaGRdB);
            break;
        case sdrplay_api_DeviceRemoved:
            flog::error("SDRPlaySourceModule '{0}': Device removed!", _this->name);
            break;
        case sdrplay_api_RspDuoModeChange:
            flog::info("SDRPlaySourceModule '{0}': RSPduo mode change", _this->name);
            break;
        case sdrplay_api_DeviceFailure:
            flog::error("SDRPlaySourceModule '{0}': Device failure!", _this->name);
            break;
        default:
            break;
        }
    }

    std::string name;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    double sampleRate;
    SourceManager::SourceHandler handler;
    bool running = false;
    double freq;
    bool initOk = false;

    sdrplay_api_CallbackFnsT cbFuncs;

    sdrplay_api_DeviceT openDev;
    sdrplay_api_DeviceParamsT* openDevParams;
    sdrplay_api_RxChannelParamsT* channelParams;

    sdrplay_api_Bw_MHzT bandwidth;
    int bandwidthId = 8; // Auto

    int devId = 0;
    int srId = 0;

    int lnaGain = 9;
    int gain = 59;
    int lnaSteps = 9;

    bool agc = false;
    bool agcParamEdit = false;
    int agcAttack = 500;
    int agcDecay = 500;
    int agcDecayDelay = 200;
    int agcDecayThreshold = 5;
    int agcSetPoint = -30;

    // Temporary values for the edit window
    int _agcAttack = 500;
    int _agcDecay = 500;
    int _agcDecayDelay = 200;
    int _agcDecayThreshold = 5;
    int _agcSetPoint = -30;

    int bufferSize = 0;
    int bufferIndex = 0;

    int ifModeId = 0;

    // General advanced options
    float ppm = 0.0f;
    bool extendedGainRange = false;
    int usbTransferMode = 0;
    int decimationFactorId = 0;
    bool decimationWideband = false;
    int dcCalMode = 3;
    int dcCalSpeedUp = 0;
    int dcCalTrackTime = 1;
    int dcCalRefreshRate = 2048;

    // RSP1A Options
    bool rsp1a_fmmwNotch = false;
    bool rsp1a_dabNotch = false;
    bool rsp1a_biasT = false;

    // RSP2 Options
    bool rsp2_fmmwNotch = false;
    bool rsp2_biasT = false;
    int rsp2_antennaPort = 0;

    // RSP Duo Options
    bool rspduo_fmmwNotch = false;
    bool rspduo_dabNotch = false;
    bool rspduo_biasT = false;
    int rspduo_antennaPort = 0;

    // RSPdx Options
    bool rspdx_fmmwNotch = false;
    bool rspdx_dabNotch = false;
    bool rspdx_biasT = false;
    int rspdx_antennaPort = 0;

    std::vector<sdrplay_api_DeviceT> devList;
    std::string devListTxt;
    std::vector<std::string> devNameList;
    std::string selectedName;

    OptionList<int, int> samplerates;
    OptionList<int, sdrplay_api_Bw_MHzT> bandwidths;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["devices"] = json({});
    def["device"] = "";
    config.setPath(core::args["root"].s() + "/sdrplay_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new SDRPlaySourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (SDRPlaySourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
