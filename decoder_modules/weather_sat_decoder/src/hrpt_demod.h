#pragma once
#include <dsp/processor.h>
#include <dsp/taps/root_raised_cosine.h>
#include <dsp/filter/fir.h>
#include <dsp/loop/fast_agc.h>
#include <dsp/loop/costas.h>
#include <dsp/clock_recovery/mm.h>

// BPSK demodulator for NOAA HRPT.
// Chain: RRC matched filter -> Fast AGC -> Costas<2> BPSK carrier recovery -> MM clock recovery
// Modeled after dsp::demod::Meteor but adapted for BPSK.
namespace dsp::demod {
    class HRPT : public Processor<complex_t, complex_t> {
        using base_type = Processor<complex_t, complex_t>;
    public:
        HRPT() {}

        ~HRPT() {
            if (!base_type::_block_init) { return; }
            base_type::stop();
            taps::free(rrcTaps);
        }

        void init(stream<complex_t>* in, double symbolrate, double samplerate,
                  int rrcTapCount, double rrcBeta, double agcRate,
                  double costasBandwidth, double omegaGain, double muGain,
                  double omegaRelLimit = 0.01) {
            _symbolrate = symbolrate;
            _samplerate = samplerate;
            _rrcTapCount = rrcTapCount;
            _rrcBeta = rrcBeta;

            rrcTaps = taps::rootRaisedCosine<float>(_rrcTapCount, _rrcBeta, _symbolrate, _samplerate);
            rrc.init(NULL, rrcTaps);
            agc.init(NULL, 1.0, 10e6, agcRate);
            costas.init(NULL, costasBandwidth);
            recov.init(NULL, _samplerate / _symbolrate, omegaGain, muGain, omegaRelLimit);

            rrc.out.free();
            agc.out.free();
            costas.out.free();
            recov.out.free();

            base_type::init(in);
        }

        void setSamplerate(double samplerate) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            _samplerate = samplerate;
            taps::free(rrcTaps);
            rrcTaps = taps::rootRaisedCosine<float>(_rrcTapCount, _rrcBeta, _symbolrate, _samplerate);
            rrc.setTaps(rrcTaps);
            recov.setOmega(_samplerate / _symbolrate);
            base_type::tempStart();
        }

        void setCostasBandwidth(double bandwidth) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            costas.setBandwidth(bandwidth);
        }

        void reset() {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            rrc.reset();
            agc.reset();
            costas.reset();
            recov.reset();
            base_type::tempStart();
        }

        inline int process(int count, const complex_t* in, complex_t* out) {
            rrc.process(count, in, out);
            agc.process(count, out, out);
            costas.process(count, out, out);
            return recov.process(count, out, out);
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            int outCount = process(count, base_type::_in->readBuf, base_type::out.writeBuf);

            base_type::_in->flush();
            if (outCount) {
                if (!base_type::out.swap(outCount)) { return -1; }
            }
            return outCount;
        }

    protected:
        double _symbolrate;
        double _samplerate;
        int _rrcTapCount;
        double _rrcBeta;

        tap<float> rrcTaps;
        filter::FIR<complex_t, float> rrc;
        loop::FastAGC<complex_t> agc;
        loop::Costas<2> costas;
        clock_recovery::MM<complex_t> recov;
    };
}
