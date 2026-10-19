// SFML Piano
//  - 21 white keys + 15 black keys (C3..B5)
//  - FL Studio style computer-keyboard layout (Z row + Q row)
//  - Polyphonic real-time synth with ADSR envelopes (no clicks / static on note end)
//  - Sustain pedal (Space), tone selection (F1-F6 or click), octave shift (Left/Right)

#include <SFML/Graphics.hpp>
#include <SFML/Audio.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

	constexpr float    kPi = 3.14159265358979f;
	constexpr unsigned kSampleRate = 44100;
	constexpr int      kChunkFrames = 512;   // ~11.6 ms per audio chunk
	constexpr int      kMaxVoices = 64;

	constexpr int kFirstMidi = 48;            // C3
	constexpr int kOctaves = 3;
	constexpr int kNumWhite = 7 * kOctaves;  // 21
	constexpr int kNumBlack = 5 * kOctaves;  // 15
	constexpr int kNumKeys = kNumWhite + kNumBlack;

	// ---------------------------------------------------------------------------
	// Tones
	// ---------------------------------------------------------------------------
	enum class Wave { Sine, Triangle, Square, Saw, Organ, Piano };

	struct Tone {
		const char* name;
		Wave  wave;
		float attack;        // seconds (linear ramp)
		float decay;         // seconds (linear ramp down to sustainLevel)
		float sustainLevel;  // 0..1
		float sustainTau;    // seconds, slow fade while held (0 = hold forever)
		float release;       // seconds, exponential time constant
		float gain;          // per-voice loudness
	};

	const Tone kTones[] = {
		{ "Sine",     Wave::Sine,     0.006f, 0.05f, 1.00f, 0.0f, 0.070f, 0.30f },
		{ "Triangle", Wave::Triangle, 0.006f, 0.05f, 1.00f, 0.0f, 0.070f, 0.30f },
		{ "Square",   Wave::Square,   0.006f, 0.08f, 0.80f, 0.0f, 0.070f, 0.13f },
		{ "Saw",      Wave::Saw,      0.006f, 0.08f, 0.80f, 0.0f, 0.070f, 0.13f },
		{ "Organ",    Wave::Organ,    0.010f, 0.02f, 1.00f, 0.0f, 0.060f, 0.17f },
		{ "Piano",    Wave::Piano,    0.003f, 0.60f, 0.35f, 3.5f, 0.130f, 0.30f },
	};
	constexpr int kNumTones = static_cast<int>(sizeof(kTones) / sizeof(kTones[0]));

	struct ToneCoeffs {
		float attackStep;
		float decayStep;
		float sustainMul;
		float releaseMul;
	};

	float polyBlep(float t, float dt) {
		if (t < dt) {
			t /= dt;
			return t + t - t * t - 1.0f;
		}
		if (t > 1.0f - dt) {
			t = (t - 1.0f) / dt;
			return t * t + t + t + 1.0f;
		}
		return 0.0f;
	}

	enum class Stage { Attack, Decay, Sustain, Release };

	struct Voice {
		bool  active = false;
		bool  keyDown = false;
		int   id = 0;
		int   tone = 0;
		float freq = 0.0f;
		float phase = 0.0f;   // 0..1
		float env = 0.0f;
		float bright = 1.0f;   // used by the piano tone: upper harmonics fade faster
		float fastRel = 0.0f;   // >0 overrides release multiplier (voice stealing)
		Stage stage = Stage::Attack;
		std::uint64_t age = 0;
	};

	// ---------------------------------------------------------------------------
	// Synth: a real-time sound stream. All voices are mixed in one continuous
	// stream, so notes never get cut off by another note and every note ends with
	// an envelope release instead of an abrupt stop (which caused the static pop).
	// ---------------------------------------------------------------------------
	class Synth : public sf::SoundStream {
	public:
		Synth() {
			initialize(2, kSampleRate, { sf::SoundChannel::FrontLeft, sf::SoundChannel::FrontRight });
			m_buffer.resize(static_cast<std::size_t>(kChunkFrames) * 2);

			for (int i = 0; i < kNumTones; ++i) {
				const Tone& t = kTones[i];
				const float sr = static_cast<float>(kSampleRate);
				m_coeffs[i].attackStep = 1.0f / (t.attack * sr);
				m_coeffs[i].decayStep = (1.0f - t.sustainLevel) / (t.decay * sr);
				m_coeffs[i].sustainMul = t.sustainTau > 0.0f ? std::exp(-1.0f / (t.sustainTau * sr)) : 1.0f;
				m_coeffs[i].releaseMul = std::exp(-1.0f / (t.release * sr));
			}
			m_brightMul = std::exp(-1.0f / (1.2f * static_cast<float>(kSampleRate)));
			m_fastReleaseMul = std::exp(-1.0f / (0.004f * static_cast<float>(kSampleRate)));
		}

		~Synth() override { stop(); }

		// Returns a voice id, or -1 if the note was dropped (all voices busy).
		int noteOn(int midi) {
			std::lock_guard<std::mutex> lock(m_mutex);
			Voice* slot = nullptr;
			for (auto& v : m_voices)
				if (!v.active) { slot = &v; break; }

			if (!slot) {
				// Steal gracefully: push the oldest voice into a very fast release
				// (no hard cut) and drop this one note.
				Voice* oldest = &m_voices[0];
				for (auto& v : m_voices)
					if (v.age < oldest->age) oldest = &v;
				oldest->stage = Stage::Release;
				oldest->fastRel = m_fastReleaseMul;
				return -1;
			}

			Voice& v = *slot;
			v = Voice{};
			v.active = true;
			v.keyDown = true;
			v.id = ++m_nextId;
			v.tone = m_tone;
			v.freq = 440.0f * std::pow(2.0f, (midi - 69) / 12.0f);
			v.stage = Stage::Attack;
			v.age = ++m_clock;
			return v.id;
		}

		void noteOff(int id) {
			if (id < 0) return;
			std::lock_guard<std::mutex> lock(m_mutex);
			for (auto& v : m_voices) {
				if (v.active && v.id == id) {
					v.keyDown = false;
					if (!m_sustain) v.stage = Stage::Release;
				}
			}
		}

		void setSustain(bool on) {
			std::lock_guard<std::mutex> lock(m_mutex);
			m_sustain = on;
			if (!on)
				for (auto& v : m_voices)
					if (v.active && !v.keyDown) v.stage = Stage::Release;
		}

		void releaseAll() {
			std::lock_guard<std::mutex> lock(m_mutex);
			m_sustain = false;
			for (auto& v : m_voices)
				if (v.active) { v.keyDown = false; v.stage = Stage::Release; }
		}

		void setTone(int tone) {
			std::lock_guard<std::mutex> lock(m_mutex);
			m_tone = ((tone % kNumTones) + kNumTones) % kNumTones;
		}

		int tone() const {
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_tone;
		}

	protected:
		bool onGetData(Chunk& data) override {
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				for (int i = 0; i < kChunkFrames; ++i) {
					const float s = std::clamp(renderSample(), -1.0f, 1.0f);
					const auto  q = static_cast<std::int16_t>(s * 32767.0f);
					m_buffer[2 * i] = q;
					m_buffer[2 * i + 1] = q;
				}
			}
			data.samples = m_buffer.data();
			data.sampleCount = m_buffer.size();
			return true;   // endless stream
		}

		void onSeek(sf::Time) override {}

	private:
		float oscillator(Voice& v, const Tone& t) const {
			const float dt = v.freq / static_cast<float>(kSampleRate);
			const float p = v.phase;
			const float twoPiP = 2.0f * kPi * p;

			switch (t.wave) {
			case Wave::Sine:
				return std::sin(twoPiP);
			case Wave::Triangle:
				return 1.0f - 4.0f * std::fabs(p - 0.5f);
			case Wave::Square: {
				float s = p < 0.5f ? 1.0f : -1.0f;
				s += polyBlep(p, dt);
				s -= polyBlep(std::fmod(p + 0.5f, 1.0f), dt);
				return s;
			}
			case Wave::Saw:
				return (2.0f * p - 1.0f) - polyBlep(p, dt);
			case Wave::Organ: {
				static const int   h[6] = { 1, 2, 3, 4, 6, 8 };
				static const float a[6] = { 1.0f, 0.8f, 0.5f, 0.4f, 0.25f, 0.15f };
				float s = 0.0f;
				for (int i = 0; i < 6; ++i)
					if (v.freq * h[i] < 18000.0f) s += a[i] * std::sin(twoPiP * h[i]);
				return s / 3.1f;
			}
			case Wave::Piano: {
				static const float a[6] = { 1.0f, 0.55f, 0.35f, 0.22f, 0.12f, 0.07f };
				float s = 0.0f, b = 1.0f;
				for (int k = 0; k < 6; ++k) {
					if (v.freq * (k + 1) < 18000.0f) s += a[k] * b * std::sin(twoPiP * (k + 1));
					b *= v.bright;   // higher harmonics fade faster over time
				}
				return s / 2.0f;
			}
			}
			return 0.0f;
		}

		float renderSample() {
			float mix = 0.0f;
			for (auto& v : m_voices) {
				if (!v.active) continue;
				const Tone& t = kTones[v.tone];
				const ToneCoeffs& c = m_coeffs[v.tone];

				switch (v.stage) {
				case Stage::Attack:
					v.env += c.attackStep;
					if (v.env >= 1.0f) { v.env = 1.0f; v.stage = Stage::Decay; }
					break;
				case Stage::Decay:
					v.env -= c.decayStep;
					if (v.env <= t.sustainLevel) { v.env = t.sustainLevel; v.stage = Stage::Sustain; }
					break;
				case Stage::Sustain:
					v.env *= c.sustainMul;
					break;
				case Stage::Release:
					v.env *= (v.fastRel > 0.0f ? v.fastRel : c.releaseMul);
					break;
				}

				// Free the voice only once it is below one 16-bit step: no audible pop.
				if (v.env < 1e-4f && v.stage != Stage::Attack) {
					v.active = false;
					continue;
				}

				if (t.wave == Wave::Piano) v.bright *= m_brightMul;

				mix += oscillator(v, t) * v.env * t.gain;

				v.phase += v.freq / static_cast<float>(kSampleRate);
				if (v.phase >= 1.0f) v.phase -= 1.0f;
			}
			return std::tanh(mix);   // soft limiter, never hard-clips
		}

		mutable std::mutex           m_mutex;
		std::array<Voice, kMaxVoices> m_voices{};
		std::array<ToneCoeffs, kNumTones> m_coeffs{};
		std::vector<std::int16_t>    m_buffer;
		float          m_brightMul = 1.0f;
		float          m_fastReleaseMul = 0.9f;
		int            m_tone = 0;
		int            m_nextId = 0;
		bool           m_sustain = false;
		std::uint64_t  m_clock = 0;
	};

	// ---------------------------------------------------------------------------
	// Keys and keyboard mapping (FL Studio layout)
	// ---------------------------------------------------------------------------
	struct PianoKey {
		int           midi = 0;
		bool          black = false;
		sf::FloatRect rect;
		bool          mouseDown = false;
		int           mouseVoice = -1;
		int           kbCount = 0;
		std::string   label;
	};

	using Scan = sf::Keyboard::Scancode;

	struct KbNote {
		Scan        scancode;
		int         semitone;   // semitones above the first C of the lower row
		const char* label;
	};

	// Lower row (Z..): C, C#, D, D#, E, F, F#, G, G#, A, A#, B, C, C#, D, D#, E
	// Upper row (Q..): same pattern starting one octave higher.
	const KbNote kKbNotes[] = {
		{ Scan::Z, 0, "Z" },  { Scan::S, 1, "S" },  { Scan::X, 2, "X" },  { Scan::D, 3, "D" },
		{ Scan::C, 4, "C" },  { Scan::V, 5, "V" },  { Scan::G, 6, "G" },  { Scan::B, 7, "B" },
		{ Scan::H, 8, "H" },  { Scan::N, 9, "N" },  { Scan::J, 10, "J" }, { Scan::M, 11, "M" },
		{ Scan::Comma, 12, "," }, { Scan::L, 13, "L" }, { Scan::Period, 14, "." },
		{ Scan::Semicolon, 15, ";" }, { Scan::Slash, 16, "/" },

		{ Scan::Q, 12, "Q" }, { Scan::Num2, 13, "2" }, { Scan::W, 14, "W" }, { Scan::Num3, 15, "3" },
		{ Scan::E, 16, "E" }, { Scan::R, 17, "R" },    { Scan::Num5, 18, "5" }, { Scan::T, 19, "T" },
		{ Scan::Num6, 20, "6" }, { Scan::Y, 21, "Y" }, { Scan::Num7, 22, "7" }, { Scan::U, 23, "U" },
		{ Scan::I, 24, "I" }, { Scan::Num9, 25, "9" }, { Scan::O, 26, "O" },    { Scan::Num0, 27, "0" },
		{ Scan::P, 28, "P" },
	};

	bool isBlackSemitone(int s) {
		s = ((s % 12) + 12) % 12;
		return s == 1 || s == 3 || s == 6 || s == 8 || s == 10;
	}

	// ---------------------------------------------------------------------------
	// Layout
	// ---------------------------------------------------------------------------
	constexpr float kWhiteW = 50.0f;
	constexpr float kWhiteH = 220.0f;
	constexpr float kBlackW = 30.0f;
	constexpr float kBlackH = 135.0f;
	constexpr float kKeysY = 70.0f;
	constexpr unsigned kWinW = static_cast<unsigned>(kNumWhite * kWhiteW);   // 1050
	constexpr unsigned kWinH = 330;

	std::optional<sf::Font> loadFont() {
		const char* candidates[] = {
			"font.ttf",
			"C:/Windows/Fonts/segoeui.ttf",
			"C:/Windows/Fonts/arial.ttf",
			"/System/Library/Fonts/Supplemental/Arial.ttf",
			"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
			"/usr/share/fonts/TTF/DejaVuSans.ttf",
		};
		for (const char* path : candidates) {
			sf::Font f;
			if (f.openFromFile(path)) return f;
		}
		return std::nullopt;
	}

} // namespace

int main() {
	sf::RenderWindow window(sf::VideoMode({ kWinW, kWinH }), "SFML Piano", sf::Style::Close);
	window.setFramerateLimit(60);
	window.setKeyRepeatEnabled(false);   // holding a key = one note, not retriggers

	std::optional<sf::Font> font = loadFont();
	if (!font) std::cerr << "No font found; labels hidden. Put a font.ttf next to the exe to show them.\n";

	Synth synth;
	synth.play();

	// ---- build keys: 21 white + 15 black -------------------------------
	std::vector<PianoKey> keys;
	keys.reserve(kNumKeys);
	std::vector<int> keyByMidi(kNumKeys, -1);   // index = midi - kFirstMidi

	int whiteIndex = 0;
	for (int s = 0; s < kOctaves * 12; ++s) {
		PianoKey k;
		k.midi = kFirstMidi + s;
		k.black = isBlackSemitone(s);
		if (!k.black) {
			k.rect = sf::FloatRect({ whiteIndex * kWhiteW, kKeysY }, { kWhiteW, kWhiteH });
			++whiteIndex;
		}
		else {
			// sits on the boundary between the previous white key and the next one
			k.rect = sf::FloatRect({ whiteIndex * kWhiteW - kBlackW / 2.0f, kKeysY }, { kBlackW, kBlackH });
		}
		keyByMidi[s] = static_cast<int>(keys.size());
		keys.push_back(k);
	}

	auto keyAt = [&](sf::Vector2f p) -> int {
		for (int i = 0; i < static_cast<int>(keys.size()); ++i)   // black keys sit on top: test first
			if (keys[i].black && keys[i].rect.contains(p)) return i;
		for (int i = 0; i < static_cast<int>(keys.size()); ++i)
			if (!keys[i].black && keys[i].rect.contains(p)) return i;
		return -1;
		};

	// ---- computer keyboard state ---------------------------------------
	int kbOctaveShift = 0;   // -1, 0, +1
	std::map<Scan, std::pair<int, int>> kbActive;   // scancode -> (key index, voice id)

	auto refreshLabels = [&]() {
		for (auto& k : keys) k.label.clear();
		for (const auto& n : kKbNotes) {
			const int idx = kbOctaveShift * 12 + n.semitone;
			if (idx < 0 || idx >= kNumKeys) continue;
			auto& label = keys[keyByMidi[idx]].label;
			if (label.empty()) label = n.label;   // lower row wins on overlap
		}
		};
	refreshLabels();

	auto kbRelease = [&](Scan sc) {
		auto it = kbActive.find(sc);
		if (it == kbActive.end()) return;
		keys[it->second.first].kbCount--;
		synth.noteOff(it->second.second);
		kbActive.erase(it);
		};

	auto mouseRelease = [&](PianoKey& k) {
		if (!k.mouseDown) return;
		k.mouseDown = false;
		synth.noteOff(k.mouseVoice);
		k.mouseVoice = -1;
		};

	int  mouseKey = -1;
	bool sustainKey = false;

	// ---- tone buttons ---------------------------------------------------
	std::array<sf::FloatRect, kNumTones> toneButtons;
	for (int i = 0; i < kNumTones; ++i)
		toneButtons[i] = sf::FloatRect({ 14.0f + i * 104.0f, 14.0f }, { 98.0f, 34.0f });

	auto setTone = [&](int t) {
		synth.setTone(t);
		std::cout << "Tone: " << kTones[synth.tone()].name << "\n";
		};

	const sf::Color kBg(24, 25, 30);
	const sf::Color kPanel(34, 36, 43);
	const sf::Color kAccent(232, 168, 56);
	const sf::Color kText(215, 217, 224);
	const sf::Color kDim(140, 144, 156);

	auto drawText = [&](const std::string& str, unsigned size, sf::Vector2f pos, sf::Color color, bool centered) {
		if (!font) return;
		sf::Text text(*font, str, size);
		text.setFillColor(color);
		if (centered) {
			const sf::FloatRect b = text.getLocalBounds();
			text.setOrigin({ b.position.x + b.size.x / 2.0f, b.position.y + b.size.y / 2.0f });
		}
		text.setPosition(pos);
		window.draw(text);
		};

	while (window.isOpen()) {
		while (const std::optional event = window.pollEvent()) {
			if (event->is<sf::Event::Closed>()) {
				window.close();
			}
			else if (event->is<sf::Event::FocusLost>()) {
				// Don't leave notes hanging when the window loses focus
				for (auto& k : keys) { k.kbCount = 0; mouseRelease(k); }
				kbActive.clear();
				mouseKey = -1;
				sustainKey = false;
				synth.releaseAll();
			}
			else if (const auto* kp = event->getIf<sf::Event::KeyPressed>()) {
				if (kp->scancode == Scan::Space) {
					sustainKey = true;
					synth.setSustain(true);
				}
				else if (kp->scancode == Scan::Left || kp->scancode == Scan::Right) {
					kbOctaveShift = std::clamp(kbOctaveShift + (kp->scancode == Scan::Right ? 1 : -1), -1, 1);
					refreshLabels();
				}
				else if (kp->code >= sf::Keyboard::Key::F1 && kp->code <= sf::Keyboard::Key::F6) {
					setTone(static_cast<int>(kp->code) - static_cast<int>(sf::Keyboard::Key::F1));
				}
				else if (kp->code == sf::Keyboard::Key::Tab) {
					setTone(synth.tone() + (kp->shift ? -1 : 1));
				}
				else if (!kbActive.count(kp->scancode)) {
					for (const auto& n : kKbNotes) {
						if (n.scancode != kp->scancode) continue;
						const int idx = kbOctaveShift * 12 + n.semitone;
						if (idx < 0 || idx >= kNumKeys) break;
						const int keyIdx = keyByMidi[idx];
						const int voice = synth.noteOn(keys[keyIdx].midi);
						keys[keyIdx].kbCount++;
						kbActive[kp->scancode] = { keyIdx, voice };
						break;
					}
				}
			}
			else if (const auto* kr = event->getIf<sf::Event::KeyReleased>()) {
				if (kr->scancode == Scan::Space) {
					sustainKey = false;
					synth.setSustain(false);
				}
				else {
					kbRelease(kr->scancode);
				}
			}
			else if (const auto* mp = event->getIf<sf::Event::MouseButtonPressed>()) {
				if (mp->button == sf::Mouse::Button::Left) {
					const sf::Vector2f pos(static_cast<float>(mp->position.x), static_cast<float>(mp->position.y));
					bool onButton = false;
					for (int i = 0; i < kNumTones; ++i)
						if (toneButtons[i].contains(pos)) { setTone(i); onButton = true; }
					if (!onButton) {
						const int idx = keyAt(pos);
						if (idx >= 0) {
							mouseKey = idx;
							keys[idx].mouseDown = true;
							keys[idx].mouseVoice = synth.noteOn(keys[idx].midi);
						}
					}
				}
			}
			else if (const auto* mm = event->getIf<sf::Event::MouseMoved>()) {
				if (mouseKey >= 0) {   // glissando: drag across keys
					const sf::Vector2f pos(static_cast<float>(mm->position.x), static_cast<float>(mm->position.y));
					const int idx = keyAt(pos);
					if (idx != mouseKey) {
						mouseRelease(keys[mouseKey]);
						mouseKey = idx;
						if (idx >= 0) {
							keys[idx].mouseDown = true;
							keys[idx].mouseVoice = synth.noteOn(keys[idx].midi);
						}
					}
				}
			}
			else if (const auto* mr = event->getIf<sf::Event::MouseButtonReleased>()) {
				if (mr->button == sf::Mouse::Button::Left && mouseKey >= 0) {
					mouseRelease(keys[mouseKey]);
					mouseKey = -1;
				}
			}
		}

		// ---- draw ------------------------------------------------------
		window.clear(kBg);

		sf::RectangleShape panel({ static_cast<float>(kWinW), 62.0f });
		panel.setFillColor(kPanel);
		window.draw(panel);

		const int currentTone = synth.tone();
		for (int i = 0; i < kNumTones; ++i) {
			sf::RectangleShape b(toneButtons[i].size);
			b.setPosition(toneButtons[i].position);
			const bool sel = (i == currentTone);
			b.setFillColor(sel ? kAccent : sf::Color(52, 55, 65));
			window.draw(b);
			drawText(std::string(kTones[i].name), 15,
				toneButtons[i].position + toneButtons[i].size / 2.0f,
				sel ? sf::Color(30, 24, 10) : kText, true);
		}

		// sustain indicator
		sf::RectangleShape sus({ 96.0f, 34.0f });
		sus.setPosition({ kWinW - 110.0f, 14.0f });
		sus.setFillColor(sustainKey ? kAccent : sf::Color(52, 55, 65));
		window.draw(sus);
		drawText("Sustain", 15, sus.getPosition() + sus.getSize() / 2.0f,
			sustainKey ? sf::Color(30, 24, 10) : kDim, true);

		// white keys first, black keys on top
		for (int pass = 0; pass < 2; ++pass) {
			for (const auto& k : keys) {
				if (k.black != (pass == 1)) continue;
				const bool down = k.mouseDown || k.kbCount > 0;

				sf::RectangleShape r(k.rect.size);
				r.setPosition(k.rect.position);
				if (!k.black) {
					r.setFillColor(down ? sf::Color(255, 214, 140) : sf::Color(244, 242, 236));
					r.setOutlineColor(sf::Color(20, 20, 24));
					r.setOutlineThickness(-1.0f);
				}
				else {
					r.setFillColor(down ? sf::Color(214, 140, 30) : sf::Color(26, 26, 31));
					r.setOutlineColor(sf::Color(0, 0, 0));
					r.setOutlineThickness(-1.0f);
				}
				window.draw(r);

				if (!k.label.empty()) {
					const sf::Vector2f c(k.rect.position.x + k.rect.size.x / 2.0f,
						k.rect.position.y + k.rect.size.y - (k.black ? 16.0f : 20.0f));
					drawText(k.label, 14, c, k.black ? sf::Color(200, 202, 210) : sf::Color(90, 92, 100), true);
				}
			}
		}

		const std::string hint = "Space: sustain     Left/Right: shift keyboard octave     F1-F6 / Tab / click: change tone";
		drawText(hint, 13, { 14.0f, kKeysY + kWhiteH + 14.0f }, kDim, false);

		window.display();
	}

	synth.stop();
	return 0;
}