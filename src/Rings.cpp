#include <string.h>
#include "AudibleInstruments.hpp"
#include "dsp/samplerate.hpp"
#include "dsp/ringbuffer.hpp"
#include "dsp/digital.hpp"
#include "rings/dsp/part.h"
#include "rings/dsp/strummer.h"
#include "rings/dsp/string_synth_part.h"


struct Rings : Module {
	enum ParamIds {
		POLYPHONY_PARAM,
		RESONATOR_PARAM,

		FREQUENCY_PARAM,
		STRUCTURE_PARAM,
		BRIGHTNESS_PARAM,
		DAMPING_PARAM,
		POSITION_PARAM,

		BRIGHTNESS_MOD_PARAM,
		FREQUENCY_MOD_PARAM,
		DAMPING_MOD_PARAM,
		STRUCTURE_MOD_PARAM,
		POSITION_MOD_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		BRIGHTNESS_MOD_INPUT,
		FREQUENCY_MOD_INPUT,
		DAMPING_MOD_INPUT,
		STRUCTURE_MOD_INPUT,
		POSITION_MOD_INPUT,

		STRUM_INPUT,
		PITCH_INPUT,
		IN_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		ODD_OUTPUT,
		EVEN_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		POLYPHONY_GREEN_LIGHT, POLYPHONY_RED_LIGHT,
		RESONATOR_GREEN_LIGHT, RESONATOR_RED_LIGHT,
		NUM_LIGHTS
	};

	SampleRateConverter<1> inputSrc;
	SampleRateConverter<2> outputSrc;
	DoubleRingBuffer<Frame<1>, 256> inputBuffer;
	DoubleRingBuffer<Frame<2>, 256> outputBuffer;

	uint16_t reverb_buffer[32768] = {};
	rings::Part part;
	rings::StringSynthPart string_synth;
	rings::Strummer strummer;
	bool strum = false;
	bool lastStrum = false;
	SchmittTrigger polyphonyTrigger;
	SchmittTrigger modelTrigger;
	int polyphonyMode = 0;
	int model = 0;

	Rings();
	void step() override;

	json_t *toJson() override {
		json_t *rootJ = json_object();

		json_object_set_new(rootJ, "polyphony", json_integer(polyphonyMode));
		json_object_set_new(rootJ, "model", json_integer(model));

		return rootJ;
	}

	void fromJson(json_t *rootJ) override {
		json_t *polyphonyJ = json_object_get(rootJ, "polyphony");
		if (polyphonyJ) {
			polyphonyMode = json_integer_value(polyphonyJ);
		}

		json_t *modelJ = json_object_get(rootJ, "model");
		if (modelJ) {
			model = json_integer_value(modelJ);
		}
	}

	void reset() override {
		polyphonyMode = 0;
		model = 0;
	}

	void randomize() override {
		polyphonyMode = randomu32() % 3;
		model = randomu32() % 3;
	}
};


Rings::Rings() : Module(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS) {
	memset(&strummer, 0, sizeof(strummer));
	memset(&part, 0, sizeof(part));
	memset(&string_synth, 0, sizeof(string_synth));

	strummer.Init(0.01, 44100.0 / 24);
	part.Init(reverb_buffer);
	string_synth.Init(reverb_buffer);

	polyphonyTrigger.setThresholds(0.0, 1.0);
	modelTrigger.setThresholds(0.0, 1.0);
}

void Rings::step() {
	// TODO
	// "Normalized to a pulse/burst generator that reacts to note changes on the V/OCT input."
	// Get input
	if (!inputBuffer.full()) {
		Frame<1> f;
		f.samples[0] = inputs[IN_INPUT].value / 5.0;
		inputBuffer.push(f);
	}

	if (!strum) {
		strum = inputs[STRUM_INPUT].value >= 1.0;
	}

	// Polyphony / model
	if (polyphonyTrigger.process(params[POLYPHONY_PARAM].value)) {
		polyphonyMode = (polyphonyMode + 1) % 3;
	}
	lights[POLYPHONY_GREEN_LIGHT].value = (polyphonyMode == 0 || polyphonyMode == 1) ? 1.0 : 0.0;
	lights[POLYPHONY_RED_LIGHT].value = (polyphonyMode == 1 || polyphonyMode == 2) ? 1.0 : 0.0;

	if (modelTrigger.process(params[RESONATOR_PARAM].value)) {
		model = (model + 1) % 3;
	}
	lights[RESONATOR_GREEN_LIGHT].value = (model == 0 || model == 1) ? 1.0 : 0.0;
	lights[RESONATOR_RED_LIGHT].value = (model == 1 || model == 2) ? 1.0 : 0.0;

	// Render frames
	if (outputBuffer.empty()) {
		float in[24] = {};
		// Convert input buffer
		{
			inputSrc.setRatio(48000.0 / engineGetSampleRate());
			int inLen = inputBuffer.size();
			int outLen = 24;
			inputSrc.process(inputBuffer.startData(), &inLen, (Frame<1>*) in, &outLen);
			inputBuffer.startIncr(inLen);
		}

		// Polyphony / model
		int polyphony = 1<<polyphonyMode;
		if (part.polyphony() != polyphony)
			part.set_polyphony(polyphony);
		part.set_model((rings::ResonatorModel)model);

		// Patch
		rings::Patch patch;
		float structure = params[STRUCTURE_PARAM].value + 3.3*quadraticBipolar(params[STRUCTURE_MOD_PARAM].value)*inputs[STRUCTURE_MOD_INPUT].value/5.0;
		patch.structure = clampf(structure, 0.0, 0.9995);
		patch.brightness = clampf(params[BRIGHTNESS_PARAM].value + 3.3*quadraticBipolar(params[BRIGHTNESS_MOD_PARAM].value)*inputs[BRIGHTNESS_MOD_INPUT].value/5.0, 0.0, 1.0);
		patch.damping = clampf(params[DAMPING_PARAM].value + 3.3*quadraticBipolar(params[DAMPING_MOD_PARAM].value)*inputs[DAMPING_MOD_INPUT].value/5.0, 0.0, 0.9995);
		patch.position = clampf(params[POSITION_PARAM].value + 3.3*quadraticBipolar(params[POSITION_MOD_PARAM].value)*inputs[POSITION_MOD_INPUT].value/5.0, 0.0, 0.9995);

		// Performance
		rings::PerformanceState performance_state;
		performance_state.note = 12.0*inputs[PITCH_INPUT].normalize(1/12.0);
		float transpose = params[FREQUENCY_PARAM].value;
		// Quantize transpose if pitch input is connected
		if (inputs[PITCH_INPUT].active) {
			transpose = roundf(transpose);
		}
		performance_state.tonic = 12.0 + clampf(transpose, 0, 60.0);
		performance_state.fm = clampf(48.0 * 3.3*quarticBipolar(params[FREQUENCY_MOD_PARAM].value) * inputs[FREQUENCY_MOD_INPUT].normalize(1.0)/5.0, -48.0, 48.0);

		performance_state.internal_exciter = !inputs[IN_INPUT].active;
		performance_state.internal_strum = !inputs[STRUM_INPUT].active;
		performance_state.internal_note = !inputs[PITCH_INPUT].active;

		// TODO
		// "Normalized to a step detector on the V/OCT input and a transient detector on the IN input."
		performance_state.strum = strum && !lastStrum;
		lastStrum = strum;
		strum = false;

		performance_state.chord = clampf(roundf(structure * (rings::kNumChords - 1)), 0, rings::kNumChords - 1);

		// Process audio
		float out[24];
		float aux[24];
		if (0) {
			// strummer.Process(NULL, 24, &performance_state);
			// string_synth.Process(performance_state, patch, in, out, aux, 24);
		}
		else {
			strummer.Process(in, 24, &performance_state);
			part.Process(performance_state, patch, in, out, aux, 24);
		}

		// Convert output buffer
		{
			Frame<2> outputFrames[24];
			for (int i = 0; i < 24; i++) {
				outputFrames[i].samples[0] = out[i];
				outputFrames[i].samples[1] = aux[i];
			}

			outputSrc.setRatio(engineGetSampleRate() / 48000.0);
			int inLen = 24;
			int outLen = outputBuffer.capacity();
			outputSrc.process(outputFrames, &inLen, outputBuffer.endData(), &outLen);
			outputBuffer.endIncr(outLen);
		}
	}

	// Set output
	if (!outputBuffer.empty()) {
		Frame<2> outputFrame = outputBuffer.shift();
		// "Note that you need to insert a jack into each output to split the signals: when only one jack is inserted, both signals are mixed together."
		if (outputs[ODD_OUTPUT].active && outputs[EVEN_OUTPUT].active) {
			outputs[ODD_OUTPUT].value = clampf(outputFrame.samples[0], -1.0, 1.0)*5.0;
			outputs[EVEN_OUTPUT].value = clampf(outputFrame.samples[1], -1.0, 1.0)*5.0;
		}
		else {
			float v = clampf(outputFrame.samples[0] + outputFrame.samples[1], -1.0, 1.0)*5.0;
			outputs[ODD_OUTPUT].value = v;
			outputs[EVEN_OUTPUT].value = v;
		}
	}
}


RingsWidget::RingsWidget() {
	Rings *module = new Rings();
	setModule(module);
	box.size = Vec(15*14, 380);

	{
		Panel *panel = new LightPanel();
		panel->backgroundImage = Image::load(assetPlugin(plugin, "res/Rings.png"));
		panel->box.size = box.size;
		addChild(panel);
	}

	addChild(createScrew<ScrewSilver>(Vec(15, 0)));
	addChild(createScrew<ScrewSilver>(Vec(180, 0)));
	addChild(createScrew<ScrewSilver>(Vec(15, 365)));
	addChild(createScrew<ScrewSilver>(Vec(180, 365)));

	addParam(createParam<TL1105>(Vec(14, 40), module, Rings::POLYPHONY_PARAM, 0.0, 1.0, 0.0));
	addParam(createParam<TL1105>(Vec(179, 40), module, Rings::RESONATOR_PARAM, 0.0, 1.0, 0.0));

	addParam(createParam<Rogan3PSWhite>(Vec(29, 72), module, Rings::FREQUENCY_PARAM, 0.0, 60.0, 30.0));
	addParam(createParam<Rogan3PSWhite>(Vec(126, 72), module, Rings::STRUCTURE_PARAM, 0.0, 1.0, 0.5));

	addParam(createParam<Rogan1PSWhite>(Vec(13, 158), module, Rings::BRIGHTNESS_PARAM, 0.0, 1.0, 0.5));
	addParam(createParam<Rogan1PSWhite>(Vec(83, 158), module, Rings::DAMPING_PARAM, 0.0, 1.0, 0.5));
	addParam(createParam<Rogan1PSWhite>(Vec(154, 158), module, Rings::POSITION_PARAM, 0.0, 1.0, 0.5));

	addParam(createParam<Trimpot>(Vec(19, 229), module, Rings::BRIGHTNESS_MOD_PARAM, -1.0, 1.0, 0.0));
	addParam(createParam<Trimpot>(Vec(57, 229), module, Rings::FREQUENCY_MOD_PARAM, -1.0, 1.0, 0.0));
	addParam(createParam<Trimpot>(Vec(96, 229), module, Rings::DAMPING_MOD_PARAM, -1.0, 1.0, 0.0));
	addParam(createParam<Trimpot>(Vec(134, 229), module, Rings::STRUCTURE_MOD_PARAM, -1.0, 1.0, 0.0));
	addParam(createParam<Trimpot>(Vec(173, 229), module, Rings::POSITION_MOD_PARAM, -1.0, 1.0, 0.0));

	addInput(createInput<PJ301MPort>(Vec(15, 273), module, Rings::BRIGHTNESS_MOD_INPUT));
	addInput(createInput<PJ301MPort>(Vec(54, 273), module, Rings::FREQUENCY_MOD_INPUT));
	addInput(createInput<PJ301MPort>(Vec(92, 273), module, Rings::DAMPING_MOD_INPUT));
	addInput(createInput<PJ301MPort>(Vec(131, 273), module, Rings::STRUCTURE_MOD_INPUT));
	addInput(createInput<PJ301MPort>(Vec(169, 273), module, Rings::POSITION_MOD_INPUT));

	addInput(createInput<PJ301MPort>(Vec(15, 316), module, Rings::STRUM_INPUT));
	addInput(createInput<PJ301MPort>(Vec(54, 316), module, Rings::PITCH_INPUT));
	addInput(createInput<PJ301MPort>(Vec(92, 316), module, Rings::IN_INPUT));
	addOutput(createOutput<PJ301MPort>(Vec(131, 316), module, Rings::ODD_OUTPUT));
	addOutput(createOutput<PJ301MPort>(Vec(169, 316), module, Rings::EVEN_OUTPUT));

	addChild(createLight<SmallLight<GreenRedLight>>(Vec(38, 43.8), module, Rings::POLYPHONY_GREEN_LIGHT));
	addChild(createLight<SmallLight<GreenRedLight>>(Vec(163, 43.8), module, Rings::RESONATOR_GREEN_LIGHT));
}
